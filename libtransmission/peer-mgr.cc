/*
 * This file Copyright (C) 2007-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <algorithm>
#include <cerrno> /* error codes ERANGE, ... */
#include <climits> /* INT_MAX */
#include <cstdlib> /* qsort */
#include <cstring> /* memcpy, memcmp, strstr */
#include <iostream>
#include <iterator>
#include <set>
#include <vector>

#include <event2/event.h>

#include <cstdint>
#include <libutp/utp.h>

#define LIBTRANSMISSION_PEER_MODULE

#include "transmission.h"
#include "announcer.h"
#include "bandwidth.h"
#include "blocklist.h"
#include "cache.h"
#include "clients.h"
#include "completion.h"
#include "crypto-utils.h"
#include "handshake.h"
#include "log.h"
#include "net.h"
#include "peer-io.h"
#include "peer-mgr.h"
#include "peer-mgr-active-requests.h"
#include "peer-mgr-wishlist.h"
#include "peer-msgs.h"
#include "ptrarray.h"
#include "session.h"
#include "stats.h" /* tr_statsAddUploaded, tr_statsAddDownloaded */
#include "torrent.h"
#include "tr-assert.h"
#include "tr-utp.h"
#include "utils.h"
#include "webseed.h"

// how frequently to cull old atoms
static auto constexpr AtomPeriodMsec = int{ 60 * 1000 };

// how frequently to change which peers are choked
static auto constexpr RechokePeriodMsec = int{ 10 * 1000 };

// an optimistically unchoked peer is immune from rechoking
// for this many calls to rechokeUploads().
static auto constexpr OptimisticUnchokeMultiplier = int{ 4 };

// how frequently to reallocate bandwidth
static auto constexpr BandwidthPeriodMsec = int{ 500 };

// how frequently to age out old piece request lists
static auto constexpr RefillUpkeepPeriodMsec = int{ 10 * 1000 };

// how frequently to decide which peers live and die
static auto constexpr ReconnectPeriodMsec = int{ 500 };

// when many peers are available, keep idle ones this long
static auto constexpr MinUploadIdleSecs = int{ 60 };

// when few peers are available, keep idle ones this long
static auto constexpr MaxUploadIdleSecs = int{ 60 * 5 };

// max number of peers to ask for per second overall.
// this throttle is to avoid overloading the router
static auto constexpr MaxConnectionsPerSecond = size_t{ 12 };

// number of bad pieces a peer is allowed to send before we ban them
static auto constexpr MaxBadPiecesPerPeer = int{ 5 };

// use for bitwise operations w/peer_atom.flags2
static auto constexpr MyflagBanned = int{ 1 };

// use for bitwise operations w/peer_atom.flags2
// unreachable for now... but not banned.
// if they try to connect to us it's okay
static auto constexpr MyflagUnreachable = int{ 2 };

// the minimum we'll wait before attempting to reconnect to a peer
static auto constexpr MinimumReconnectIntervalSecs = int{ 5 };

// how long we'll let requests we've made linger before we cancel them
static auto constexpr RequestTtlSecs = int{ 90 };

static auto constexpr CancelHistorySec = int{ 60 };

/**
***
**/

/**
 * Peer information that should be kept even before we've connected and
 * after we've disconnected. These are kept in a pool of peer_atoms to decide
 * which ones would make good candidates for connecting to, and to watch out
 * for banned peers.
 *
 * @see tr_peer
 * @see tr_peerMsgs
 */
struct peer_atom
{
    uint8_t fromFirst; /* where the peer was first found */
    uint8_t fromBest; /* the "best" value of where the peer has been found */
    uint8_t flags; /* these match the added_f flags */
    uint8_t flags2; /* flags that aren't defined in added_f */
    int8_t blocklisted; /* -1 for unknown, true for blocklisted, false for not blocklisted */

    tr_port port;
    bool utp_failed; /* We recently failed to connect over uTP */
    uint16_t numFails;
    time_t time; /* when the peer's connection status last changed */
    time_t piece_data_time;

    time_t lastConnectionAttemptAt;
    time_t lastConnectionAt;

    /* similar to a TTL field, but less rigid --
     * if the swarm is small, the atom will be kept past this date. */
    time_t shelf_date;
    tr_peer* peer; /* will be nullptr if not connected */
    tr_address addr;
};

#ifndef TR_ENABLE_ASSERTS

#define tr_isAtom(a) (true)

#else

static bool tr_isAtom(struct peer_atom const* atom)
{
    return atom != nullptr && atom->fromFirst < TR_PEER_FROM__MAX && atom->fromBest < TR_PEER_FROM__MAX &&
        tr_address_is_valid(&atom->addr);
}

#endif

static char const* tr_atomAddrStr(struct peer_atom const* atom)
{
    static char addrstr[TR_ADDRSTRLEN];
    return atom != nullptr ? tr_address_and_port_to_string(addrstr, sizeof(addrstr), &atom->addr, atom->port) : "[no atom]";
}

/** @brief Opaque, per-torrent data structure for peer connection information */
class tr_swarm
{
public:
    tr_swarm(tr_peerMgr* manager_in, tr_torrent* tor_in)
        : manager{ manager_in }
        , tor{ tor_in }
    {
    }

public:
    tr_swarm_stats stats = {};

    tr_ptrArray outgoingHandshakes = {}; /* tr_handshake */
    tr_ptrArray pool = {}; /* struct peer_atom */
    tr_ptrArray peers = {}; /* tr_peerMsgs */
    tr_ptrArray webseeds = {}; /* tr_webseed */

    tr_peerMgr* const manager;
    tr_torrent* const tor;

    tr_peerMsgs* optimistic = nullptr; /* the optimistic peer, or nullptr if none */
    int optimisticUnchokeTimeScaler = 0;

    bool poolIsAllSeeds = false;
    bool poolIsAllSeedsDirty = true; /* true if poolIsAllSeeds needs to be recomputed */
    bool isRunning = false;
    bool needsCompletenessCheck = true;
    bool endgame = false;

    ActiveRequests active_requests;
    Wishlist wishlist;

    int interestedCount = 0;
    int maxPeers = 0;
    time_t lastCancel = 0;
};

struct tr_peerMgr
{
    auto unique_lock() const
    {
        return session->unique_lock();
    }

    tr_session* session;
    tr_ptrArray incomingHandshakes; /* tr_handshake */
    struct event* bandwidthTimer;
    struct event* rechokeTimer;
    struct event* refillUpkeepTimer;
    struct event* atomTimer;
};

#define tordbg(t, ...) tr_logAddDeepNamed(tr_torrentName((t)->tor), __VA_ARGS__)

#define dbgmsg(...) tr_logAddDeepNamed(nullptr, __VA_ARGS__)

/**
*** tr_peer virtual functions
**/

unsigned int tr_peerGetPieceSpeed_Bps(tr_peer const* peer, uint64_t now, tr_direction direction)
{
    unsigned int Bps = 0;
    peer->is_transferring_pieces(now, direction, &Bps);
    return Bps;
}

tr_peer::tr_peer(tr_torrent const* tor, peer_atom* atom_in)
    : session{ tor->session }
    , atom{ atom_in }
    , swarm{ tor->swarm }
    , blame{ tor->n_blocks }
    , have{ tor->info.pieceCount }
{
}

tr_peer::~tr_peer()
{
    if (swarm != nullptr)
    {
        swarm->active_requests.remove(this);
    }

    if (atom != nullptr)
    {
        atom->peer = nullptr;
    }
}

/**
***
**/

static int handshakeCompareToAddr(void const* va, void const* vb)
{
    auto const* const a = static_cast<tr_handshake const*>(va);
    auto const* const b = static_cast<tr_address const*>(vb);

    return tr_address_compare(tr_handshakeGetAddr(a, nullptr), b);
}

static int handshakeCompare(void const* va, void const* vb)
{
    auto const* const b = static_cast<tr_handshake const*>(vb);
    return handshakeCompareToAddr(va, tr_handshakeGetAddr(b, nullptr));
}

static inline tr_handshake* getExistingHandshake(tr_ptrArray* handshakes, tr_address const* addr)
{
    if (tr_ptrArrayEmpty(handshakes))
    {
        return nullptr;
    }

    return static_cast<tr_handshake*>(tr_ptrArrayFindSorted(handshakes, addr, handshakeCompareToAddr));
}

static int comparePeerAtomToAddress(void const* va, void const* vb)
{
    auto const* const a = static_cast<struct peer_atom const*>(va);
    auto const* const b = static_cast<tr_address const*>(vb);

    return tr_address_compare(&a->addr, b);
}

static int compareAtomsByAddress(void const* va, void const* vb)
{
    auto const* const b = static_cast<struct peer_atom const*>(vb);

    TR_ASSERT(tr_isAtom(b));

    return comparePeerAtomToAddress(va, &b->addr);
}

/**
***
**/

tr_address const* tr_peerAddress(tr_peer const* peer)
{
    return &peer->atom->addr;
}

static tr_swarm* getExistingSwarm(tr_peerMgr* manager, uint8_t const* hash)
{
    tr_torrent* tor = tr_torrentFindFromHash(manager->session, hash);

    return tor == nullptr ? nullptr : tor->swarm;
}

static int peerCompare(void const* va, void const* vb)
{
    auto const* const a = static_cast<tr_peer const*>(va);
    auto const* const b = static_cast<tr_peer const*>(vb);
    return tr_address_compare(tr_peerAddress(a), tr_peerAddress(b));
}

static struct peer_atom* getExistingAtom(tr_swarm const* cswarm, tr_address const* addr)
{
    auto* swarm = const_cast<tr_swarm*>(cswarm);
    return static_cast<struct peer_atom*>(tr_ptrArrayFindSorted(&swarm->pool, addr, comparePeerAtomToAddress));
}

static bool peerIsInUse(tr_swarm const* cs, struct peer_atom const* atom)
{
    auto* s = const_cast<tr_swarm*>(cs);
    auto const lock = s->manager->unique_lock();

    return atom->peer != nullptr || getExistingHandshake(&s->outgoingHandshakes, &atom->addr) != nullptr ||
        getExistingHandshake(&s->manager->incomingHandshakes, &atom->addr) != nullptr;
}

static void swarmFree(void* vs)
{
    auto* s = static_cast<tr_swarm*>(vs);
    TR_ASSERT(s != nullptr);
    auto const lock = s->manager->unique_lock();

    TR_ASSERT(!s->isRunning);
    TR_ASSERT(tr_ptrArrayEmpty(&s->outgoingHandshakes));
    TR_ASSERT(tr_ptrArrayEmpty(&s->peers));

    tr_ptrArrayDestruct(&s->webseeds, [](void* peer) { delete static_cast<tr_peer*>(peer); });
    tr_ptrArrayDestruct(&s->pool, (PtrArrayForeachFunc)tr_free);
    tr_ptrArrayDestruct(&s->outgoingHandshakes, nullptr);
    tr_ptrArrayDestruct(&s->peers, nullptr);
    s->stats = {};

    delete s;
}

static void peerCallbackFunc(tr_peer*, tr_peer_event const*, void*);

static void rebuildWebseedArray(tr_swarm* s, tr_torrent* tor)
{
    tr_info const* inf = &tor->info;

    /* clear the array */
    tr_ptrArrayDestruct(&s->webseeds, [](void* peer) { delete static_cast<tr_peer*>(peer); });
    s->webseeds = {};
    s->stats.activeWebseedCount = 0;

    /* repopulate it */
    for (unsigned int i = 0; i < inf->webseedCount; ++i)
    {
        tr_peer* w = tr_webseedNew(tor, inf->webseeds[i], peerCallbackFunc, s);
        tr_ptrArrayAppend(&s->webseeds, w);
    }
}

static tr_swarm* swarmNew(tr_peerMgr* manager, tr_torrent* tor)
{
    auto* swarm = new tr_swarm{ manager, tor };

    rebuildWebseedArray(swarm, tor);

    return swarm;
}

static void ensureMgrTimersExist(struct tr_peerMgr* m);

tr_peerMgr* tr_peerMgrNew(tr_session* session)
{
    tr_peerMgr* m = tr_new0(tr_peerMgr, 1);
    m->session = session;
    m->incomingHandshakes = {};
    ensureMgrTimersExist(m);
    return m;
}

static void deleteTimer(struct event** t)
{
    if (*t != nullptr)
    {
        event_free(*t);
        *t = nullptr;
    }
}

static void deleteTimers(struct tr_peerMgr* m)
{
    deleteTimer(&m->atomTimer);
    deleteTimer(&m->bandwidthTimer);
    deleteTimer(&m->rechokeTimer);
    deleteTimer(&m->refillUpkeepTimer);
}

void tr_peerMgrFree(tr_peerMgr* manager)
{
    auto const lock = manager->unique_lock();

    deleteTimers(manager);

    /* free the handshakes. Abort invokes handshakeDoneCB(), which removes
     * the item from manager->handshakes, so this is a little roundabout... */
    while (!tr_ptrArrayEmpty(&manager->incomingHandshakes))
    {
        tr_handshakeAbort(static_cast<tr_handshake*>(tr_ptrArrayNth(&manager->incomingHandshakes, 0)));
    }

    tr_ptrArrayDestruct(&manager->incomingHandshakes, nullptr);

    tr_free(manager);
}

/***
****
***/

void tr_peerMgrOnBlocklistChanged(tr_peerMgr* mgr)
{
    /* we cache whether or not a peer is blocklisted...
       since the blocklist has changed, erase that cached value */
    for (auto* tor : mgr->session->torrents)
    {
        tr_swarm* s = tor->swarm;

        for (int i = 0, n = tr_ptrArraySize(&s->pool); i < n; ++i)
        {
            auto* const atom = static_cast<struct peer_atom*>(tr_ptrArrayNth(&s->pool, i));
            atom->blocklisted = -1;
        }
    }
}

static bool isAtomBlocklisted(tr_session const* session, struct peer_atom* atom)
{
    if (atom->blocklisted < 0)
    {
        atom->blocklisted = (int8_t)tr_sessionIsAddressBlocked(session, &atom->addr);
    }

    return atom->blocklisted != 0;
}

/***
****
***/

static constexpr bool atomIsSeed(struct peer_atom const* atom)
{
    return (atom != nullptr) && ((atom->flags & ADDED_F_SEED_FLAG) != 0);
}

static void atomSetSeed(tr_swarm* s, struct peer_atom* atom)
{
    tordbg(s, "marking peer %s as a seed", tr_atomAddrStr(atom));
    atom->flags |= ADDED_F_SEED_FLAG;
    s->poolIsAllSeedsDirty = true;
}

bool tr_peerMgrPeerIsSeed(tr_torrent const* tor, tr_address const* addr)
{
    bool isSeed = false;
    tr_swarm const* s = tor->swarm;
    struct peer_atom const* atom = getExistingAtom(s, addr);

    if (atom != nullptr)
    {
        isSeed = atomIsSeed(atom);
    }

    return isSeed;
}

void tr_peerMgrSetUtpSupported(tr_torrent* tor, tr_address const* addr)
{
    struct peer_atom* atom = getExistingAtom(tor->swarm, addr);

    if (atom != nullptr)
    {
        atom->flags |= ADDED_F_UTP_FLAGS;
    }
}

void tr_peerMgrSetUtpFailed(tr_torrent* tor, tr_address const* addr, bool failed)
{
    struct peer_atom* atom = getExistingAtom(tor->swarm, addr);

    if (atom != nullptr)
    {
        atom->utp_failed = failed;
    }
}

/**
***  REQUESTS
***
*** There are two data structures associated with managing block requests:
***
*** 1. tr_swarm::active_requests, an opaque class that tracks what requests
***    we currently have, i.e. which blocks and from which peers.
***    This is used for cancelling requests that have been waiting
***    for too long and avoiding duplicate requests.
***
*** 2. tr_swarm::pieces, an array of "struct weighted_piece" which lists the
***    pieces that we want to request. It's used to decide which blocks to
***    return next when tr_peerMgrGetBlockRequests() is called.
**/

/**
*** struct block_request
**/

static int countActiveWebseeds(tr_swarm* s)
{
    int activeCount = 0;

    if (s->tor->isRunning && !tr_torrentIsSeed(s->tor))
    {
        uint64_t const now = tr_time_msec();

        for (int i = 0, n = tr_ptrArraySize(&s->webseeds); i < n; ++i)
        {
            if (static_cast<tr_peer const*>(tr_ptrArrayNth(&s->webseeds, i))->is_transferring_pieces(now, TR_DOWN, nullptr))
            {
                ++activeCount;
            }
        }
    }

    return activeCount;
}

// TODO: if we keep this, add equivalent API to ActiveRequest
void tr_peerMgrClientSentRequests(tr_torrent* torrent, tr_peer* peer, tr_block_span_t span)
{
    auto const now = tr_time();

    for (tr_block_index_t block = span.begin; block < span.end; ++block)
    {
        torrent->swarm->active_requests.add(block, peer, now);
    }
}

static void updateEndgame(tr_swarm* s)
{
    /* we consider ourselves to be in endgame if the number of bytes
       we've got requested is >= the number of bytes left to download */
    s->endgame = uint64_t(std::size(s->active_requests)) * s->tor->block_size >= s->tor->leftUntilDone();
}

std::vector<tr_block_span_t> tr_peerMgrGetNextRequests(tr_torrent* torrent, tr_peer const* peer, size_t numwant)
{
    class PeerInfoImpl : public Wishlist::PeerInfo
    {
    public:
        PeerInfoImpl(tr_torrent const* torrent_in, tr_peer const* peer_in)
            : torrent_{ torrent_in }
            , swarm_{ torrent_in->swarm }
            , peer_{ peer_in }
        {
        }

        ~PeerInfoImpl() override = default;

        bool clientCanRequestBlock(tr_block_index_t block) const override
        {
            return !torrent_->hasBlock(block) && !swarm_->active_requests.has(block, peer_);
        }

        bool clientCanRequestPiece(tr_piece_index_t piece) const override
        {
            return torrent_->pieceIsWanted(piece) && peer_->have.test(piece);
        }

        bool isEndgame() const override
        {
            return swarm_->endgame;
        }

        size_t countActiveRequests(tr_block_index_t block) const override
        {
            return swarm_->active_requests.count(block);
        }

        size_t countMissingBlocks(tr_piece_index_t piece) const override
        {
            return torrent_->countMissingBlocksInPiece(piece);
        }

        tr_block_span_t blockSpan(tr_piece_index_t piece) const override
        {
            return torrent_->blockSpanForPiece(piece);
        }

        tr_piece_index_t countAllPieces() const override
        {
            return torrent_->info.pieceCount;
        }

        tr_priority_t priority(tr_piece_index_t piece) const override
        {
            return torrent_->piecePriority(piece);
        }

    private:
        tr_torrent const* const torrent_;
        tr_swarm const* const swarm_;
        tr_peer const* const peer_;
    };

    auto* const swarm = torrent->swarm;
    updateEndgame(swarm);
    return swarm->wishlist.next(PeerInfoImpl(torrent, peer), numwant);
}

/****
*****
*****  Piece List Manipulation / Accessors
*****
****/

bool tr_peerMgrDidPeerRequest(tr_torrent const* tor, tr_peer const* peer, tr_block_index_t block)
{
    return tor->swarm->active_requests.has(block, peer);
}

size_t tr_peerMgrCountActiveRequestsToPeer(tr_torrent const* tor, tr_peer const* peer)
{
    return tor->swarm->active_requests.count(peer);
}

static void maybeSendCancelRequest(tr_peer* peer, tr_block_index_t block, tr_peer const* muted)
{
    auto* msgs = dynamic_cast<tr_peerMsgs*>(peer);
    if (msgs != nullptr && msgs != muted)
    {
        peer->cancelsSentToPeer.add(tr_time(), 1);
        msgs->cancel_block_request(block);
    }
}

static void cancelAllRequestsForBlock(tr_swarm* swarm, tr_block_index_t block, tr_peer const* no_notify)
{
    for (auto* peer : swarm->active_requests.remove(block))
    {
        maybeSendCancelRequest(peer, block, no_notify);
    }
}

static void tr_swarmCancelOldRequests(tr_swarm* swarm)
{
    auto const now = tr_time();
    auto const oldest = now - RequestTtlSecs;

    for (auto const& [block, peer] : swarm->active_requests.sentBefore(oldest))
    {
        maybeSendCancelRequest(peer, block, nullptr);
        swarm->active_requests.remove(block, peer);
    }
}

static void refillUpkeep(evutil_socket_t /*fd*/, short /*what*/, void* vmgr)
{
    auto* mgr = static_cast<tr_peerMgr*>(vmgr);
    auto const lock = mgr->unique_lock();

    auto& torrents = mgr->session->torrents;
    std::for_each(std::begin(torrents), std::end(torrents), [](auto* tor) { tr_swarmCancelOldRequests(tor->swarm); });

    tr_timerAddMsec(mgr->refillUpkeepTimer, RefillUpkeepPeriodMsec);
}

static void addStrike(tr_swarm* s, tr_peer* peer)
{
    tordbg(s, "increasing peer %s strike count to %d", tr_atomAddrStr(peer->atom), peer->strikes + 1);

    if (++peer->strikes >= MaxBadPiecesPerPeer)
    {
        struct peer_atom* atom = peer->atom;
        atom->flags2 |= MyflagBanned;
        peer->doPurge = true;
        tordbg(s, "banning peer %s", tr_atomAddrStr(atom));
    }
}

static void peerSuggestedPiece(tr_swarm* /*s*/, tr_peer* /*peer*/, tr_piece_index_t /*pieceIndex*/, int /*isFastAllowed*/)
{
#if 0

    TR_ASSERT(t != nullptr);
    TR_ASSERT(peer != nullptr);
    TR_ASSERT(peer->msgs != nullptr);

    /* is this a valid piece? */
    if (pieceIndex >= t->tor->info.pieceCount)
    {
        return;
    }

    /* don't ask for it if we've already got it */
    if (t->tor->hasPiece(pieceIndex))
    {
        return;
    }

    /* don't ask for it if they don't have it */
    if (!peer->have.readBit(pieceIndex))
    {
        return;
    }

    /* don't ask for it if we're choked and it's not fast */
    if (!isFastAllowed && peer->clientIsChoked)
    {
        return;
    }

    /* request the blocks that we don't have in this piece */
    {
        tr_torrent const* tor = t->tor;
        auto const [begin, end] = tor->blockSpanForPiece(pieceIndex);

        for (tr_block_index_t b = begin; b < end; ++b)
        {
            if (tor->hasBlock(b))
            {
                uint32_t const offset = getBlockOffsetInPiece(tor, b);
                uint32_t const length = tor->blockSize(b);
                tr_peerMsgsAddRequest(peer->msgs, pieceIndex, offset, length);
                incrementPieceRequests(t, pieceIndex);
            }
        }
    }
#endif
}

void tr_peerMgrPieceCompleted(tr_torrent* tor, tr_piece_index_t p)
{
    bool pieceCameFromPeers = false;
    tr_swarm* const s = tor->swarm;

    /* walk through our peers */
    for (int i = 0, n = tr_ptrArraySize(&s->peers); i < n; ++i)
    {
        auto* peer = static_cast<tr_peerMsgs*>(tr_ptrArrayNth(&s->peers, i));

        // notify the peer that we now have this piece
        peer->on_piece_completed(p);

        if (!pieceCameFromPeers)
        {
            pieceCameFromPeers = peer->blame.test(p);
        }
    }

    if (pieceCameFromPeers) /* webseed downloads don't belong in announce totals */
    {
        tr_announcerAddBytes(tor, TR_ANN_DOWN, tor->pieceSize(p));
    }

    /* bookkeeping */
    s->needsCompletenessCheck = true;
}

static void peerCallbackFunc(tr_peer* peer, tr_peer_event const* e, void* vs)
{
    TR_ASSERT(peer != nullptr);
    auto* s = static_cast<tr_swarm*>(vs);
    auto const lock = s->manager->unique_lock();

    switch (e->eventType)
    {
    case TR_PEER_PEER_GOT_PIECE_DATA:
        {
            time_t const now = tr_time();
            tr_torrent* tor = s->tor;

            tor->uploadedCur += e->length;
            tr_announcerAddBytes(tor, TR_ANN_UP, e->length);
            tr_torrentSetDateActive(tor, now);
            tr_torrentSetDirty(tor);
            tr_statsAddUploaded(tor->session, e->length);

            if (peer->atom != nullptr)
            {
                peer->atom->piece_data_time = now;
            }

            break;
        }

    case TR_PEER_CLIENT_GOT_PIECE_DATA:
        {
            time_t const now = tr_time();
            tr_torrent* tor = s->tor;

            tor->downloadedCur += e->length;
            tr_torrentSetDateActive(tor, now);
            tr_torrentSetDirty(tor);

            tr_statsAddDownloaded(tor->session, e->length);

            if (peer->atom != nullptr)
            {
                peer->atom->piece_data_time = now;
            }

            break;
        }

    case TR_PEER_CLIENT_GOT_HAVE:
    case TR_PEER_CLIENT_GOT_HAVE_ALL:
    case TR_PEER_CLIENT_GOT_HAVE_NONE:
    case TR_PEER_CLIENT_GOT_BITFIELD:
        /* TODO: if we don't need these, should these events be removed? */
        /* noop */
        break;

    case TR_PEER_CLIENT_GOT_REJ:
        s->active_requests.remove(s->tor->blockOf(e->pieceIndex, e->offset), peer);
        break;

    case TR_PEER_CLIENT_GOT_CHOKE:
        s->active_requests.remove(peer);
        break;

    case TR_PEER_CLIENT_GOT_PORT:
        if (peer->atom != nullptr)
        {
            peer->atom->port = e->port;
        }

        break;

    case TR_PEER_CLIENT_GOT_SUGGEST:
        peerSuggestedPiece(s, peer, e->pieceIndex, false);
        break;

    case TR_PEER_CLIENT_GOT_ALLOWED_FAST:
        peerSuggestedPiece(s, peer, e->pieceIndex, true);
        break;

    case TR_PEER_CLIENT_GOT_BLOCK:
        {
            tr_torrent* tor = s->tor;
            tr_piece_index_t const p = e->pieceIndex;
            tr_block_index_t const block = tor->blockOf(p, e->offset);
            cancelAllRequestsForBlock(s, block, peer);
            peer->blocksSentToClient.add(tr_time(), 1);
            tr_torrentGotBlock(tor, block);
            break;
        }

    case TR_PEER_ERROR:
        if (e->err == ERANGE || e->err == EMSGSIZE || e->err == ENOTCONN)
        {
            /* some protocol error from the peer */
            peer->doPurge = true;
            tordbg(
                s,
                "setting %s doPurge flag because we got an ERANGE, EMSGSIZE, or ENOTCONN error",
                tr_atomAddrStr(peer->atom));
        }
        else
        {
            tordbg(s, "unhandled error: %s", tr_strerror(e->err));
        }

        break;

    default:
        TR_ASSERT_MSG(false, "unhandled peer event type %d", (int)e->eventType);
    }
}

static int getDefaultShelfLife(uint8_t from)
{
    /* in general, peers obtained from firsthand contact
     * are better than those from secondhand, etc etc */
    switch (from)
    {
    case TR_PEER_FROM_INCOMING:
        return 60 * 60 * 6;

    case TR_PEER_FROM_LTEP:
        return 60 * 60 * 6;

    case TR_PEER_FROM_TRACKER:
        return 60 * 60 * 3;

    case TR_PEER_FROM_DHT:
        return 60 * 60 * 3;

    case TR_PEER_FROM_PEX:
        return 60 * 60 * 2;

    case TR_PEER_FROM_RESUME:
        return 60 * 60;

    case TR_PEER_FROM_LPD:
        return 10 * 60;

    default:
        return 60 * 60;
    }
}

static struct peer_atom* ensureAtomExists(
    tr_swarm* s,
    tr_address const* addr,
    tr_port const port,
    uint8_t const flags,
    uint8_t const from)
{
    TR_ASSERT(tr_address_is_valid(addr));
    TR_ASSERT(from < TR_PEER_FROM__MAX);

    struct peer_atom* a = getExistingAtom(s, addr);

    if (a == nullptr)
    {
        int const jitter = tr_rand_int_weak(60 * 10);
        a = tr_new0(struct peer_atom, 1);
        a->addr = *addr;
        a->port = port;
        a->flags = flags;
        a->fromFirst = from;
        a->fromBest = from;
        a->shelf_date = tr_time() + getDefaultShelfLife(from) + jitter;
        a->blocklisted = -1;
        tr_ptrArrayInsertSorted(&s->pool, a, compareAtomsByAddress);

        tordbg(s, "got a new atom: %s", tr_atomAddrStr(a));
    }
    else
    {
        if (from < a->fromBest)
        {
            a->fromBest = from;
        }

        a->flags |= flags;
    }

    s->poolIsAllSeedsDirty = true;

    return a;
}

static int getMaxPeerCount(tr_torrent const* tor)
{
    return tor->maxConnectedPeers;
}

static int getPeerCount(tr_swarm const* s)
{
    return tr_ptrArraySize(&s->peers);
}

static void createBitTorrentPeer(tr_torrent* tor, tr_peerIo* io, struct peer_atom* atom, tr_quark client)
{
    TR_ASSERT(atom != nullptr);
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(tor->swarm != nullptr);

    tr_swarm* swarm = tor->swarm;

    auto* peer = tr_peerMsgsNew(tor, atom, io, peerCallbackFunc, swarm);
    peer->client = client;
    atom->peer = peer;

    tr_ptrArrayInsertSorted(&swarm->peers, peer, peerCompare);
    ++swarm->stats.peerCount;
    ++swarm->stats.peerFromCount[atom->fromFirst];

    TR_ASSERT(swarm->stats.peerCount == tr_ptrArraySize(&swarm->peers));
    TR_ASSERT(swarm->stats.peerFromCount[atom->fromFirst] <= swarm->stats.peerCount);

    // TODO is this needed?
    // isn't it already initialized in tr_peerMsgsImpl's ctor?
    peer->update_active(TR_UP);
    peer->update_active(TR_DOWN);
}

/* FIXME: this is kind of a mess. */
static bool on_handshake_done(tr_handshake_result const& result)
{
    TR_ASSERT(result.io != nullptr);

    bool ok = result.isConnected;
    bool success = false;
    auto* manager = static_cast<tr_peerMgr*>(result.userData);
    tr_swarm* s = tr_peerIoHasTorrentHash(result.io) ? getExistingSwarm(manager, tr_peerIoGetTorrentHash(result.io)) : nullptr;

    if (tr_peerIoIsIncoming(result.io))
    {
        tr_ptrArrayRemoveSortedPointer(&manager->incomingHandshakes, result.handshake, handshakeCompare);
    }
    else if (s != nullptr)
    {
        tr_ptrArrayRemoveSortedPointer(&s->outgoingHandshakes, result.handshake, handshakeCompare);
    }

    auto const lock = manager->unique_lock();

    auto port = tr_port{};
    tr_address const* const addr = tr_peerIoGetAddress(result.io, &port);

    if (!ok || s == nullptr || !s->isRunning)
    {
        if (s != nullptr)
        {
            struct peer_atom* atom = getExistingAtom(s, addr);

            if (atom != nullptr)
            {
                ++atom->numFails;

                if (!result.readAnythingFromPeer)
                {
                    tordbg(s, "marking peer %s as unreachable... numFails is %d", tr_atomAddrStr(atom), (int)atom->numFails);
                    atom->flags2 |= MyflagUnreachable;
                }
            }
        }
    }
    else /* looking good */
    {
        struct peer_atom* atom = ensureAtomExists(s, addr, port, 0, TR_PEER_FROM_INCOMING);

        atom->time = tr_time();
        atom->piece_data_time = 0;
        atom->lastConnectionAt = tr_time();

        if (!tr_peerIoIsIncoming(result.io))
        {
            atom->flags |= ADDED_F_CONNECTABLE;
            atom->flags2 &= ~MyflagUnreachable;
        }

        /* In principle, this flag specifies whether the peer groks uTP,
           not whether it's currently connected over uTP. */
        if (result.io->socket.type == TR_PEER_SOCKET_TYPE_UTP)
        {
            atom->flags |= ADDED_F_UTP_FLAGS;
        }

        if ((atom->flags2 & MyflagBanned) != 0)
        {
            tordbg(s, "banned peer %s tried to reconnect", tr_atomAddrStr(atom));
        }
        else if (tr_peerIoIsIncoming(result.io) && getPeerCount(s) >= getMaxPeerCount(s->tor))
        {
            /* too many peers already */
        }
        else
        {
            tr_peer const* const peer = atom->peer;

            if (peer != nullptr)
            {
                /* we already have this peer */
            }
            else
            {
                auto client = tr_quark{ TR_KEY_NONE };
                if (result.peer_id)
                {
                    char buf[128] = {};
                    tr_clientForId(buf, sizeof(buf), *result.peer_id);
                    client = tr_quark_new(buf);
                }

                /* this steals its refcount too, which is balanced by our unref in peerDelete() */
                tr_peerIo* stolen = tr_handshakeStealIO(result.handshake);
                tr_peerIoSetParent(stolen, s->tor->bandwidth);
                createBitTorrentPeer(s->tor, stolen, atom, client);

                success = true;
            }
        }
    }

    return success;
}

static void close_peer_socket(struct tr_peer_socket const socket, tr_session* session)
{
    switch (socket.type)
    {
    case TR_PEER_SOCKET_TYPE_NONE:
        break;

    case TR_PEER_SOCKET_TYPE_TCP:
        tr_netClose(session, socket.handle.tcp);
        break;

#ifdef WITH_UTP

    case TR_PEER_SOCKET_TYPE_UTP:
        UTP_Close(socket.handle.utp);
        break;

#endif

    default:
        TR_ASSERT_MSG(false, "unsupported peer socket type %d", socket.type);
    }
}

void tr_peerMgrAddIncoming(tr_peerMgr* manager, tr_address* addr, tr_port port, struct tr_peer_socket const socket)
{
    TR_ASSERT(tr_isSession(manager->session));
    auto const lock = manager->unique_lock();

    tr_session* session = manager->session;

    if (tr_sessionIsAddressBlocked(session, addr))
    {
        tr_logAddDebug("Banned IP address \"%s\" tried to connect to us", tr_address_to_string(addr));
        close_peer_socket(socket, session);
    }
    else if (getExistingHandshake(&manager->incomingHandshakes, addr) != nullptr)
    {
        close_peer_socket(socket, session);
    }
    else /* we don't have a connection to them yet... */
    {
        tr_peerIo* const io = tr_peerIoNewIncoming(session, session->bandwidth, addr, port, socket);
        tr_handshake* const handshake = tr_handshakeNew(io, session->encryptionMode, on_handshake_done, manager);

        tr_peerIoUnref(io); /* balanced by the implicit ref in tr_peerIoNewIncoming() */

        tr_ptrArrayInsertSorted(&manager->incomingHandshakes, handshake, handshakeCompare);
    }
}

void tr_peerMgrSetSwarmIsAllSeeds(tr_torrent* tor)
{
    auto const lock = tor->unique_lock();

    tr_swarm* const swarm = tor->swarm;
    auto atomCount = int{};
    struct peer_atom** atoms = (struct peer_atom**)tr_ptrArrayPeek(&swarm->pool, &atomCount);
    for (int i = 0; i < atomCount; ++i)
    {
        atomSetSeed(swarm, atoms[i]);
    }

    swarm->poolIsAllSeeds = true;
    swarm->poolIsAllSeedsDirty = false;
}

size_t tr_peerMgrAddPex(tr_torrent* tor, uint8_t from, tr_pex const* pex, size_t n_pex)
{
    size_t n_used = 0;
    tr_swarm* s = tor->swarm;
    auto const lock = s->manager->unique_lock();

    for (tr_pex const* const end = pex + n_pex; pex != end; ++pex)
    {
        if (tr_isPex(pex) && /* safeguard against corrupt data */
            !tr_sessionIsAddressBlocked(s->manager->session, &pex->addr) &&
            tr_address_is_valid_for_peers(&pex->addr, pex->port))
        {
            ensureAtomExists(s, &pex->addr, pex->port, pex->flags, from);
            ++n_used;
        }
    }

    return n_used;
}

tr_pex* tr_peerMgrCompactToPex(
    void const* compact,
    size_t compactLen,
    uint8_t const* added_f,
    size_t added_f_len,
    size_t* pexCount)
{
    size_t n = compactLen / 6;
    auto const* walk = static_cast<uint8_t const*>(compact);
    tr_pex* pex = tr_new0(tr_pex, n);

    for (size_t i = 0; i < n; ++i)
    {
        pex[i].addr.type = TR_AF_INET;
        memcpy(&pex[i].addr.addr, walk, 4);
        walk += 4;
        memcpy(&pex[i].port, walk, 2);
        walk += 2;

        if (added_f != nullptr && n == added_f_len)
        {
            pex[i].flags = added_f[i];
        }
    }

    *pexCount = n;
    return pex;
}

tr_pex* tr_peerMgrCompact6ToPex(
    void const* compact,
    size_t compactLen,
    uint8_t const* added_f,
    size_t added_f_len,
    size_t* pexCount)
{
    size_t n = compactLen / 18;
    auto const* walk = static_cast<uint8_t const*>(compact);
    tr_pex* pex = tr_new0(tr_pex, n);

    for (size_t i = 0; i < n; ++i)
    {
        pex[i].addr.type = TR_AF_INET6;
        memcpy(&pex[i].addr.addr.addr6.s6_addr, walk, 16);
        walk += 16;
        memcpy(&pex[i].port, walk, 2);
        walk += 2;

        if (added_f != nullptr && n == added_f_len)
        {
            pex[i].flags = added_f[i];
        }
    }

    *pexCount = n;
    return pex;
}

/**
***
**/

void tr_peerMgrGotBadPiece(tr_torrent* tor, tr_piece_index_t pieceIndex)
{
    tr_swarm* s = tor->swarm;
    uint32_t const byteCount = tor->pieceSize(pieceIndex);

    for (int i = 0, n = tr_ptrArraySize(&s->peers); i != n; ++i)
    {
        auto* const peer = static_cast<tr_peer*>(tr_ptrArrayNth(&s->peers, i));

        if (peer->blame.test(pieceIndex))
        {
            tordbg(
                s,
                "peer %s contributed to corrupt piece (%d); now has %d strikes",
                tr_atomAddrStr(peer->atom),
                pieceIndex,
                (int)peer->strikes + 1);
            addStrike(s, peer);
        }
    }

    tr_announcerAddBytes(tor, TR_ANN_CORRUPT, byteCount);
}

int tr_pexCompare(void const* va, void const* vb)
{
    auto const* const a = static_cast<tr_pex const*>(va);
    auto const* const b = static_cast<tr_pex const*>(vb);

    TR_ASSERT(tr_isPex(a));
    TR_ASSERT(tr_isPex(b));

    auto i = int{};

    if ((i = tr_address_compare(&a->addr, &b->addr)) != 0)
    {
        return i;
    }

    if (a->port != b->port)
    {
        return a->port < b->port ? -1 : 1;
    }

    return 0;
}

/* better goes first */
static int compareAtomsByUsefulness(void const* va, void const* vb)
{
    struct peer_atom const* a = *(struct peer_atom const* const*)va;
    struct peer_atom const* b = *(struct peer_atom const* const*)vb;

    TR_ASSERT(tr_isAtom(a));
    TR_ASSERT(tr_isAtom(b));

    if (a->piece_data_time != b->piece_data_time)
    {
        return a->piece_data_time > b->piece_data_time ? -1 : 1;
    }

    if (a->fromBest != b->fromBest)
    {
        return a->fromBest < b->fromBest ? -1 : 1;
    }

    if (a->numFails != b->numFails)
    {
        return a->numFails < b->numFails ? -1 : 1;
    }

    return 0;
}

static bool isAtomInteresting(tr_torrent const* tor, struct peer_atom* atom)
{
    if (tr_torrentIsSeed(tor) && atomIsSeed(atom))
    {
        return false;
    }

    if (peerIsInUse(tor->swarm, atom))
    {
        return true;
    }

    if (isAtomBlocklisted(tor->session, atom))
    {
        return false;
    }

    if ((atom->flags2 & MyflagBanned) != 0)
    {
        return false;
    }

    return true;
}

// TODO: return a std::vector
int tr_peerMgrGetPeers(tr_torrent const* tor, tr_pex** setme_pex, uint8_t af, uint8_t list_mode, int maxCount)
{
    TR_ASSERT(tr_isTorrent(tor));
    auto const lock = tor->unique_lock();

    TR_ASSERT(setme_pex != nullptr);
    TR_ASSERT(af == TR_AF_INET || af == TR_AF_INET6);
    TR_ASSERT(list_mode == TR_PEERS_CONNECTED || list_mode == TR_PEERS_INTERESTING);

    tr_swarm const* s = tor->swarm;

    /**
    ***  build a list of atoms
    **/

    auto atomCount = int{};
    struct peer_atom** atoms = nullptr;
    if (list_mode == TR_PEERS_CONNECTED) /* connected peers only */
    {
        tr_peer const** peers = (tr_peer const**)tr_ptrArrayBase(&s->peers);
        atomCount = tr_ptrArraySize(&s->peers);
        atoms = tr_new(struct peer_atom*, atomCount);

        for (int i = 0; i < atomCount; ++i)
        {
            atoms[i] = peers[i]->atom;
        }
    }
    else /* TR_PEERS_INTERESTING */
    {
        struct peer_atom** atomBase = (struct peer_atom**)tr_ptrArrayBase(&s->pool);
        int const n = tr_ptrArraySize(&s->pool);
        atoms = tr_new(struct peer_atom*, n);

        for (int i = 0; i < n; ++i)
        {
            if (isAtomInteresting(tor, atomBase[i]))
            {
                atoms[atomCount++] = atomBase[i];
            }
        }
    }

    qsort(atoms, atomCount, sizeof(struct peer_atom*), compareAtomsByUsefulness);

    /**
    ***  add the first N of them into our return list
    **/

    int const n = std::min(atomCount, maxCount);
    tr_pex* const pex = tr_new0(tr_pex, n);
    tr_pex* walk = pex;

    auto count = int{};
    for (int i = 0; i < atomCount && count < n; ++i)
    {
        struct peer_atom const* atom = atoms[i];

        if (atom->addr.type == af)
        {
            TR_ASSERT(tr_address_is_valid(&atom->addr));

            walk->addr = atom->addr;
            walk->port = atom->port;
            walk->flags = atom->flags;
            ++count;
            ++walk;
        }
    }

    qsort(pex, count, sizeof(tr_pex), tr_pexCompare);

    TR_ASSERT(walk - pex == count);
    *setme_pex = pex;

    /* cleanup */
    tr_free(atoms);
    return count;
}

static void atomPulse(evutil_socket_t, short, void*);
static void bandwidthPulse(evutil_socket_t, short, void*);
static void rechokePulse(evutil_socket_t, short, void*);
static void reconnectPulse(evutil_socket_t, short, void*);

static struct event* createTimer(tr_session* session, int msec, event_callback_fn callback, void* cbdata)
{
    struct event* timer = evtimer_new(session->event_base, callback, cbdata);
    tr_timerAddMsec(timer, msec);
    return timer;
}

static void ensureMgrTimersExist(struct tr_peerMgr* m)
{
    if (m->atomTimer == nullptr)
    {
        m->atomTimer = createTimer(m->session, AtomPeriodMsec, atomPulse, m);
    }

    if (m->bandwidthTimer == nullptr)
    {
        m->bandwidthTimer = createTimer(m->session, BandwidthPeriodMsec, bandwidthPulse, m);
    }

    if (m->rechokeTimer == nullptr)
    {
        m->rechokeTimer = createTimer(m->session, RechokePeriodMsec, rechokePulse, m);
    }

    if (m->refillUpkeepTimer == nullptr)
    {
        m->refillUpkeepTimer = createTimer(m->session, RefillUpkeepPeriodMsec, refillUpkeep, m);
    }
}

void tr_peerMgrStartTorrent(tr_torrent* tor)
{
    TR_ASSERT(tr_isTorrent(tor));
    auto const lock = tor->unique_lock();

    tr_swarm* s = tor->swarm;

    ensureMgrTimersExist(s->manager);

    s->isRunning = true;
    s->maxPeers = tor->maxConnectedPeers;

    // rechoke soon
    tr_timerAddMsec(s->manager->rechokeTimer, 100);
}

static void removeAllPeers(tr_swarm*);

static void stopSwarm(tr_swarm* swarm)
{
    swarm->isRunning = false;

    removeAllPeers(swarm);

    /* disconnect the handshakes. handshakeAbort calls handshakeDoneCB(),
     * which removes the handshake from t->outgoingHandshakes... */
    while (!tr_ptrArrayEmpty(&swarm->outgoingHandshakes))
    {
        tr_handshakeAbort(static_cast<tr_handshake*>(tr_ptrArrayNth(&swarm->outgoingHandshakes, 0)));
    }
}

void tr_peerMgrStopTorrent(tr_torrent* tor)
{
    TR_ASSERT(tr_isTorrent(tor));
    auto const lock = tor->unique_lock();

    stopSwarm(tor->swarm);
}

void tr_peerMgrAddTorrent(tr_peerMgr* manager, tr_torrent* tor)
{
    TR_ASSERT(tr_isTorrent(tor));
    auto const lock = tor->unique_lock();
    TR_ASSERT(tor->swarm == nullptr);

    tor->swarm = swarmNew(manager, tor);
}

void tr_peerMgrRemoveTorrent(tr_torrent* tor)
{
    TR_ASSERT(tr_isTorrent(tor));
    auto const lock = tor->unique_lock();

    stopSwarm(tor->swarm);
    swarmFree(tor->swarm);
}

void tr_peerUpdateProgress(tr_torrent* tor, tr_peer* peer)
{
    auto const* have = &peer->have;

    if (have->hasAll())
    {
        peer->progress = 1.0;
    }
    else if (have->hasNone())
    {
        peer->progress = 0.0;
    }
    else
    {
        float const true_count = have->count();

        if (tr_torrentHasMetadata(tor))
        {
            peer->progress = true_count / tor->info.pieceCount;
        }
        else /* without pieceCount, this result is only a best guess... */
        {
            peer->progress = true_count / static_cast<float>(have->size() + 1);
        }
    }

    peer->progress = std::clamp(peer->progress, 0.0F, 1.0F);

    if (peer->atom != nullptr && peer->progress >= 1.0f)
    {
        atomSetSeed(tor->swarm, peer->atom);
    }
}

void tr_peerMgrOnTorrentGotMetainfo(tr_torrent* tor)
{
    /* the webseed list may have changed... */
    rebuildWebseedArray(tor->swarm, tor);

    /* some peer_msgs' progress fields may not be accurate if we
       didn't have the metadata before now... so refresh them all... */
    int const peerCount = tr_ptrArraySize(&tor->swarm->peers);
    tr_peer** const peers = (tr_peer**)tr_ptrArrayBase(&tor->swarm->peers);

    for (int i = 0; i < peerCount; ++i)
    {
        tr_peerUpdateProgress(tor, peers[i]);
    }

    /* update the bittorrent peers' willingnes... */
    for (int i = 0; i < peerCount; ++i)
    {
        auto* msgs = static_cast<tr_peerMsgs*>(peers[i]);
        msgs->update_active(TR_UP);
        msgs->update_active(TR_DOWN);
    }
}

void tr_peerMgrTorrentAvailability(tr_torrent const* tor, int8_t* tab, unsigned int tabCount)
{
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(tab != nullptr);
    TR_ASSERT(tabCount > 0);

    memset(tab, 0, tabCount);

    if (tr_torrentHasMetadata(tor))
    {
        int const peerCount = tr_ptrArraySize(&tor->swarm->peers);
        tr_peer const** peers = (tr_peer const**)tr_ptrArrayBase(&tor->swarm->peers);
        float const interval = tor->info.pieceCount / (float)tabCount;
        bool const isSeed = tr_torrentGetCompleteness(tor) == TR_SEED;

        for (tr_piece_index_t i = 0; i < tabCount; ++i)
        {
            int const piece = i * interval;

            if (isSeed || tor->hasPiece(piece))
            {
                tab[i] = -1;
            }
            else if (peerCount != 0)
            {
                for (int j = 0; j < peerCount; ++j)
                {
                    if (peers[j]->have.test(piece))
                    {
                        ++tab[i];
                    }
                }
            }
        }
    }
}

void tr_swarmGetStats(tr_swarm const* swarm, tr_swarm_stats* setme)
{
    TR_ASSERT(swarm != nullptr);
    TR_ASSERT(setme != nullptr);

    *setme = swarm->stats;
}

void tr_swarmIncrementActivePeers(tr_swarm* swarm, tr_direction direction, bool is_active)
{
    int n = swarm->stats.activePeerCount[direction];

    if (is_active)
    {
        ++n;
    }
    else
    {
        --n;
    }

    TR_ASSERT(n >= 0);
    TR_ASSERT(n <= swarm->stats.peerCount);

    swarm->stats.activePeerCount[direction] = n;
}

bool tr_peerIsSeed(tr_peer const* peer)
{
    return (peer != nullptr) && ((peer->progress >= 1.0) || atomIsSeed(peer->atom));
}

/* count how many bytes we want that connected peers have */
uint64_t tr_peerMgrGetDesiredAvailable(tr_torrent const* tor)
{
    TR_ASSERT(tr_isTorrent(tor));

    // common shortcuts...

    if (!tor->isRunning || tor->isStopping || tr_torrentIsSeed(tor) || !tr_torrentHasMetadata(tor))
    {
        return 0;
    }

    tr_swarm const* const s = tor->swarm;
    if (s == nullptr || !s->isRunning)
    {
        return 0;
    }

    size_t const n_peers = tr_ptrArraySize(&s->peers);
    if (n_peers == 0)
    {
        return 0;
    }

    tr_peer const** const peers = (tr_peer const**)tr_ptrArrayBase(&s->peers);
    for (size_t i = 0; i < n_peers; ++i)
    {
        if (peers[i]->atom != nullptr && atomIsSeed(peers[i]->atom))
        {
            return tor->leftUntilDone();
        }
    }

    // do it the hard way

    auto desired_available = uint64_t{};
    auto const n_pieces = tor->info.pieceCount;
    auto have = std::vector<bool>(n_pieces);

    for (size_t i = 0; i < n_peers; ++i)
    {
        auto* peer = peers[i];
        for (size_t j = 0; j < n_pieces; ++j)
        {
            if (peer->have.test(j))
            {
                have[j] = true;
            }
        }
    }

    for (size_t i = 0; i < n_pieces; ++i)
    {
        if (tor->pieceIsWanted(i) && have.at(i))
        {
            desired_available += tor->countMissingBytesInPiece(i);
        }
    }

    TR_ASSERT(desired_available <= tor->info.totalSize);
    return desired_available;
}

double* tr_peerMgrWebSpeeds_KBps(tr_torrent const* tor)
{
    TR_ASSERT(tr_isTorrent(tor));

    auto const now = tr_time_msec();

    tr_swarm* const s = tor->swarm;
    TR_ASSERT(s->manager != nullptr);

    unsigned int n = tr_ptrArraySize(&s->webseeds);
    TR_ASSERT(n == tor->info.webseedCount);

    double* ret = tr_new0(double, n);

    for (unsigned int i = 0; i < n; ++i)
    {
        unsigned int Bps = 0;
        auto const* const peer = static_cast<tr_peer*>(tr_ptrArrayNth(&s->webseeds, i));

        if (peer->is_transferring_pieces(now, TR_DOWN, &Bps))
        {
            ret[i] = Bps / (double)tr_speed_K;
        }
        else
        {
            ret[i] = -1.0;
        }
    }

    return ret;
}

static auto getPeerStats(tr_peerMsgs const* peer, time_t now, uint64_t now_msec)
{
    auto stats = tr_peer_stat{};
    auto const* const atom = peer->atom;

    tr_address_to_string_with_buf(&atom->addr, stats.addr, sizeof(stats.addr));
    tr_strlcpy(stats.client, tr_quark_get_string(peer->client), sizeof(stats.client));
    stats.port = ntohs(peer->atom->port);
    stats.from = atom->fromFirst;
    stats.progress = peer->progress;
    stats.isUTP = peer->is_utp_connection();
    stats.isEncrypted = peer->is_encrypted();
    stats.rateToPeer_KBps = toSpeedKBps(tr_peerGetPieceSpeed_Bps(peer, now_msec, TR_CLIENT_TO_PEER));
    stats.rateToClient_KBps = toSpeedKBps(tr_peerGetPieceSpeed_Bps(peer, now_msec, TR_PEER_TO_CLIENT));
    stats.peerIsChoked = peer->is_peer_choked();
    stats.peerIsInterested = peer->is_peer_interested();
    stats.clientIsChoked = peer->is_client_choked();
    stats.clientIsInterested = peer->is_client_interested();
    stats.isIncoming = peer->is_incoming_connection();
    stats.isDownloadingFrom = peer->is_active(TR_PEER_TO_CLIENT);
    stats.isUploadingTo = peer->is_active(TR_CLIENT_TO_PEER);
    stats.isSeed = tr_peerIsSeed(peer);

    stats.blocksToPeer = peer->blocksSentToPeer.count(now, CancelHistorySec);
    stats.blocksToClient = peer->blocksSentToClient.count(now, CancelHistorySec);
    stats.cancelsToPeer = peer->cancelsSentToPeer.count(now, CancelHistorySec);
    stats.cancelsToClient = peer->cancelsSentToClient.count(now, CancelHistorySec);

    stats.pendingReqsToPeer = peer->swarm->active_requests.count(peer);
    stats.pendingReqsToClient = peer->pendingReqsToClient;

    char* pch = stats.flagStr;

    if (stats.isUTP)
    {
        *pch++ = 'T';
    }

    if (peer->swarm->optimistic == peer)
    {
        *pch++ = 'O';
    }

    if (stats.isDownloadingFrom)
    {
        *pch++ = 'D';
    }
    else if (stats.clientIsInterested)
    {
        *pch++ = 'd';
    }

    if (stats.isUploadingTo)
    {
        *pch++ = 'U';
    }
    else if (stats.peerIsInterested)
    {
        *pch++ = 'u';
    }

    if (!stats.clientIsChoked && !stats.clientIsInterested)
    {
        *pch++ = 'K';
    }

    if (!stats.peerIsChoked && !stats.peerIsInterested)
    {
        *pch++ = '?';
    }

    if (stats.isEncrypted)
    {
        *pch++ = 'E';
    }

    if (stats.from == TR_PEER_FROM_DHT)
    {
        *pch++ = 'H';
    }
    else if (stats.from == TR_PEER_FROM_PEX)
    {
        *pch++ = 'X';
    }

    if (stats.isIncoming)
    {
        *pch++ = 'I';
    }

    *pch = '\0';

    return stats;
}

struct tr_peer_stat* tr_peerMgrPeerStats(tr_torrent const* tor, int* setmeCount)
{
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(tor->swarm->manager != nullptr);

    tr_peerMsgs** peers = (tr_peerMsgs**)tr_ptrArrayBase(&tor->swarm->peers);
    int const size = tr_ptrArraySize(&tor->swarm->peers);
    tr_peer_stat* ret = tr_new0(tr_peer_stat, size);

    time_t const now = tr_time();
    uint64_t const now_msec = tr_time_msec();
    for (int i = 0; i < size; ++i)
    {
        ret[i] = getPeerStats(peers[i], now, now_msec);
    }

    *setmeCount = size;
    return ret;
}

/***
****
****
***/

void tr_peerMgrClearInterest(tr_torrent* tor)
{
    TR_ASSERT(tr_isTorrent(tor));
    auto const lock = tor->unique_lock();

    tr_swarm* s = tor->swarm;
    int const peerCount = tr_ptrArraySize(&s->peers);

    for (int i = 0; i < peerCount; ++i)
    {
        static_cast<tr_peerMsgs*>(tr_ptrArrayNth(&s->peers, i))->set_interested(false);
    }
}

/* does this peer have any pieces that we want? */
static bool isPeerInteresting(tr_torrent* const tor, bool const* const piece_is_interesting, tr_peer const* const peer)
{
    /* these cases should have already been handled by the calling code... */
    TR_ASSERT(!tr_torrentIsSeed(tor));
    TR_ASSERT(tr_torrentIsPieceTransferAllowed(tor, TR_PEER_TO_CLIENT));

    if (tr_peerIsSeed(peer))
    {
        return true;
    }

    for (tr_piece_index_t i = 0; i < tor->info.pieceCount; ++i)
    {
        if (piece_is_interesting[i] && peer->have.test(i))
        {
            return true;
        }
    }

    return false;
}

enum tr_rechoke_state
{
    RECHOKE_STATE_GOOD,
    RECHOKE_STATE_UNTESTED,
    RECHOKE_STATE_BAD
};

struct tr_rechoke_info
{
    tr_peerMsgs* peer;
    int salt;
    int rechoke_state;
};

static constexpr int compare_rechoke_info(void const* va, void const* vb)
{
    auto const* const a = static_cast<struct tr_rechoke_info const*>(va);
    auto const* const b = static_cast<struct tr_rechoke_info const*>(vb);

    if (a->rechoke_state != b->rechoke_state)
    {
        return a->rechoke_state - b->rechoke_state;
    }

    return a->salt - b->salt;
}

/* determines who we send "interested" messages to */
static void rechokeDownloads(tr_swarm* s)
{
    int maxPeers = 0;
    int rechoke_count = 0;
    struct tr_rechoke_info* rechoke = nullptr;
    auto constexpr MinInterestingPeers = 5;
    int const peerCount = tr_ptrArraySize(&s->peers);
    time_t const now = tr_time();

    /* some cases where this function isn't necessary */
    if (tr_torrentIsSeed(s->tor))
    {
        return;
    }

    if (!tr_torrentIsPieceTransferAllowed(s->tor, TR_PEER_TO_CLIENT))
    {
        return;
    }

    /* decide HOW MANY peers to be interested in */
    {
        int blocks = 0;
        int cancels = 0;

        /* Count up how many blocks & cancels each peer has.
         *
         * There are two situations where we send out cancels --
         *
         * 1. We've got unresponsive peers, which is handled by deciding
         *    -which- peers to be interested in.
         *
         * 2. We've hit our bandwidth cap, which is handled by deciding
         *    -how many- peers to be interested in.
         *
         * We're working on 2. here, so we need to ignore unresponsive
         * peers in our calculations lest they confuse Transmission into
         * thinking it's hit its bandwidth cap.
         */
        for (int i = 0; i < peerCount; ++i)
        {
            auto const* const peer = static_cast<tr_peer const*>(tr_ptrArrayNth(&s->peers, i));
            auto const b = peer->blocksSentToClient.count(now, CancelHistorySec);
            auto const c = peer->cancelsSentToPeer.count(now, CancelHistorySec);

            if (b == 0) /* ignore unresponsive peers, as described above */
            {
                continue;
            }

            blocks += b;
            cancels += c;
        }

        if (cancels > 0)
        {
            /* cancelRate: of the block requests we've recently made, the percentage we cancelled.
             * higher values indicate more congestion. */
            double const cancelRate = cancels / (double)(cancels + blocks);
            double const mult = 1 - std::min(cancelRate, 0.5);
            maxPeers = s->interestedCount * mult;
            tordbg(
                s,
                "cancel rate is %.3f -- reducing the number of peers we're interested in by %.0f percent",
                cancelRate,
                mult * 100);
            s->lastCancel = now;
        }

        time_t const timeSinceCancel = now - s->lastCancel;

        if (timeSinceCancel != 0)
        {
            int const maxIncrease = 15;
            time_t const maxHistory = 2 * CancelHistorySec;
            double const mult = std::min(timeSinceCancel, maxHistory) / (double)maxHistory;
            int const inc = maxIncrease * mult;
            maxPeers = s->maxPeers + inc;
            tordbg(
                s,
                "time since last cancel is %jd -- increasing the number of peers we're interested in by %d",
                (intmax_t)timeSinceCancel,
                inc);
        }
    }

    /* don't let the previous section's number tweaking go too far... */
    maxPeers = std::clamp(maxPeers, MinInterestingPeers, int(s->tor->maxConnectedPeers));

    s->maxPeers = maxPeers;

    if (peerCount > 0)
    {
        tr_torrent const* const tor = s->tor;
        int const n = tor->info.pieceCount;

        /* build a bitfield of interesting pieces... */
        bool* const piece_is_interesting = tr_new(bool, n);

        for (int i = 0; i < n; ++i)
        {
            piece_is_interesting[i] = tor->pieceIsWanted(i) && !tor->hasPiece(i);
        }

        /* decide WHICH peers to be interested in (based on their cancel-to-block ratio) */
        for (int i = 0; i < peerCount; ++i)
        {
            auto* const peer = static_cast<tr_peerMsgs*>(tr_ptrArrayNth(&s->peers, i));

            if (!isPeerInteresting(s->tor, piece_is_interesting, peer))
            {
                peer->set_interested(false);
            }
            else
            {
                auto rechoke_state = tr_rechoke_state{};
                auto const blocks = peer->blocksSentToClient.count(now, CancelHistorySec);
                auto const cancels = peer->cancelsSentToPeer.count(now, CancelHistorySec);

                if (blocks == 0 && cancels == 0)
                {
                    rechoke_state = RECHOKE_STATE_UNTESTED;
                }
                else if (cancels == 0)
                {
                    rechoke_state = RECHOKE_STATE_GOOD;
                }
                else if (blocks == 0)
                {
                    rechoke_state = RECHOKE_STATE_BAD;
                }
                else if (cancels * 10 < blocks)
                {
                    rechoke_state = RECHOKE_STATE_GOOD;
                }
                else
                {
                    rechoke_state = RECHOKE_STATE_BAD;
                }

                if (rechoke == nullptr)
                {
                    rechoke = tr_new(struct tr_rechoke_info, peerCount);
                }

                rechoke[rechoke_count].peer = peer;
                rechoke[rechoke_count].rechoke_state = rechoke_state;
                rechoke[rechoke_count].salt = tr_rand_int_weak(INT_MAX);
                rechoke_count++;
            }
        }

        tr_free(piece_is_interesting);
    }

    if ((rechoke != nullptr) && (rechoke_count > 0))
    {
        qsort(rechoke, rechoke_count, sizeof(struct tr_rechoke_info), compare_rechoke_info);
    }

    /* now that we know which & how many peers to be interested in... update the peer interest */

    s->interestedCount = std::min(maxPeers, rechoke_count);

    for (int i = 0; i < rechoke_count; ++i)
    {
        rechoke[i].peer->set_interested(i < s->interestedCount);
    }

    /* cleanup */
    tr_free(rechoke);
}

/**
***
**/

struct ChokeData
{
    bool isInterested;
    bool wasChoked;
    bool isChoked;
    int rate;
    int salt;
    tr_peerMsgs* msgs;
};

static int compareChoke(void const* va, void const* vb)
{
    auto const* const a = static_cast<struct ChokeData const*>(va);
    auto const* const b = static_cast<struct ChokeData const*>(vb);

    if (a->rate != b->rate) /* prefer higher overall speeds */
    {
        return a->rate > b->rate ? -1 : 1;
    }

    if (a->wasChoked != b->wasChoked) /* prefer unchoked */
    {
        return a->wasChoked ? 1 : -1;
    }

    if (a->salt != b->salt) /* random order */
    {
        return a->salt - b->salt;
    }

    return 0;
}

/* is this a new connection? */
static bool isNew(tr_peerMsgs const* msgs)
{
    return msgs != nullptr && msgs->get_connection_age() < 45;
}

/* get a rate for deciding which peers to choke and unchoke. */
static int getRate(tr_torrent const* tor, struct peer_atom* atom, uint64_t now)
{
    auto Bps = unsigned{};

    if (tr_torrentIsSeed(tor))
    {
        Bps = tr_peerGetPieceSpeed_Bps(atom->peer, now, TR_CLIENT_TO_PEER);
    }
    /* downloading a private torrent... take upload speed into account
     * because there may only be a small window of opportunity to share */
    else if (tr_torrentIsPrivate(tor))
    {
        Bps = tr_peerGetPieceSpeed_Bps(atom->peer, now, TR_PEER_TO_CLIENT) +
            tr_peerGetPieceSpeed_Bps(atom->peer, now, TR_CLIENT_TO_PEER);
    }
    /* downloading a public torrent */
    else
    {
        Bps = tr_peerGetPieceSpeed_Bps(atom->peer, now, TR_PEER_TO_CLIENT);
    }

    /* convert it to bytes per second */
    return Bps;
}

static inline bool isBandwidthMaxedOut(Bandwidth const* b, uint64_t const now_msec, tr_direction dir)
{
    if (!b->isLimited(dir))
    {
        return false;
    }

    unsigned int const got = b->getPieceSpeedBytesPerSecond(now_msec, dir);
    unsigned int const want = b->getDesiredSpeedBytesPerSecond(dir);
    return got >= want;
}

static void rechokeUploads(tr_swarm* s, uint64_t const now)
{
    auto const lock = s->manager->unique_lock();

    int const peerCount = tr_ptrArraySize(&s->peers);
    tr_peerMsgs** peers = (tr_peerMsgs**)tr_ptrArrayBase(&s->peers);
    struct ChokeData* choke = tr_new0(struct ChokeData, peerCount);
    tr_session const* session = s->manager->session;
    bool const chokeAll = !tr_torrentIsPieceTransferAllowed(s->tor, TR_CLIENT_TO_PEER);
    bool const isMaxedOut = isBandwidthMaxedOut(s->tor->bandwidth, now, TR_UP);

    /* an optimistic unchoke peer's "optimistic"
     * state lasts for N calls to rechokeUploads(). */
    if (s->optimisticUnchokeTimeScaler > 0)
    {
        s->optimisticUnchokeTimeScaler--;
    }
    else
    {
        s->optimistic = nullptr;
    }

    int size = 0;

    /* sort the peers by preference and rate */
    for (int i = 0; i < peerCount; ++i)
    {
        auto* const peer = peers[i];
        struct peer_atom* const atom = peer->atom;

        if (tr_peerIsSeed(peer))
        {
            /* choke seeds and partial seeds */
            peer->set_choke(true);
        }
        else if (chokeAll)
        {
            /* choke everyone if we're not uploading */
            peer->set_choke(true);
        }
        else if (peer != s->optimistic)
        {
            struct ChokeData* n = &choke[size++];
            n->msgs = peer;
            n->isInterested = peer->is_peer_interested();
            n->wasChoked = peer->is_peer_choked();
            n->rate = getRate(s->tor, atom, now);
            n->salt = tr_rand_int_weak(INT_MAX);
            n->isChoked = true;
        }
    }

    qsort(choke, size, sizeof(struct ChokeData), compareChoke);

    /**
     * Reciprocation and number of uploads capping is managed by unchoking
     * the N peers which have the best upload rate and are interested.
     * This maximizes the client's download rate. These N peers are
     * referred to as downloaders, because they are interested in downloading
     * from the client.
     *
     * Peers which have a better upload rate (as compared to the downloaders)
     * but aren't interested get unchoked. If they become interested, the
     * downloader with the worst upload rate gets choked. If a client has
     * a complete file, it uses its upload rate rather than its download
     * rate to decide which peers to unchoke.
     *
     * If our bandwidth is maxed out, don't unchoke any more peers.
     */
    int checkedChokeCount = 0;
    int unchokedInterested = 0;

    for (int i = 0; i < size && unchokedInterested < session->uploadSlotsPerTorrent; ++i)
    {
        choke[i].isChoked = isMaxedOut ? choke[i].wasChoked : false;

        ++checkedChokeCount;

        if (choke[i].isInterested)
        {
            ++unchokedInterested;
        }
    }

    /* optimistic unchoke */
    if (s->optimistic == nullptr && !isMaxedOut && checkedChokeCount < size)
    {
        auto randPool = std::vector<ChokeData*>{};

        for (int i = checkedChokeCount; i < size; ++i)
        {
            if (choke[i].isInterested)
            {
                tr_peerMsgs const* msgs = choke[i].msgs;
                int const x = isNew(msgs) ? 3 : 1;

                for (int y = 0; y < x; ++y)
                {
                    randPool.push_back(&choke[i]);
                }
            }
        }

        auto const n = std::size(randPool);
        if (n != 0)
        {
            auto* c = randPool[tr_rand_int_weak(n)];
            c->isChoked = false;
            s->optimistic = c->msgs;
            s->optimisticUnchokeTimeScaler = OptimisticUnchokeMultiplier;
        }
    }

    for (int i = 0; i < size; ++i)
    {
        choke[i].msgs->set_choke(choke[i].isChoked);
    }

    /* cleanup */
    tr_free(choke);
}

static void rechokePulse(evutil_socket_t /*fd*/, short /*what*/, void* vmgr)
{
    auto* mgr = static_cast<tr_peerMgr*>(vmgr);
    auto const lock = mgr->unique_lock();
    uint64_t const now = tr_time_msec();

    for (auto* tor : mgr->session->torrents)
    {
        if (tor->isRunning)
        {
            tr_swarm* s = tor->swarm;

            if (s->stats.peerCount > 0)
            {
                rechokeUploads(s, now);
                rechokeDownloads(s);
            }
        }
    }

    tr_timerAddMsec(mgr->rechokeTimer, RechokePeriodMsec);
}

/***
****
****  Life and Death
****
***/

static bool shouldPeerBeClosed(tr_swarm const* s, tr_peer const* peer, int peerCount, time_t const now)
{
    tr_torrent const* tor = s->tor;
    struct peer_atom const* atom = peer->atom;

    /* if it's marked for purging, close it */
    if (peer->doPurge)
    {
        tordbg(s, "purging peer %s because its doPurge flag is set", tr_atomAddrStr(atom));
        return true;
    }

    /* disconnect if we're both seeds and enough time has passed for PEX */
    if (tr_torrentIsSeed(tor) && tr_peerIsSeed(peer))
    {
        return !tr_torrentAllowsPex(tor) || now - atom->time >= 30;
    }

    /* disconnect if it's been too long since piece data has been transferred.
     * this is on a sliding scale based on number of available peers... */
    {
        int const relaxStrictnessIfFewerThanN = (int)(getMaxPeerCount(tor) * 0.9 + 0.5);
        /* if we have >= relaxIfFewerThan, strictness is 100%.
         * if we have zero connections, strictness is 0% */
        float const strictness = peerCount >= relaxStrictnessIfFewerThanN ? 1.0 :
                                                                            peerCount / (float)relaxStrictnessIfFewerThanN;
        int const lo = MinUploadIdleSecs;
        int const hi = MaxUploadIdleSecs;
        int const limit = hi - (hi - lo) * strictness;
        int const idleTime = now - std::max(atom->time, atom->piece_data_time);

        if (idleTime > limit)
        {
            tordbg(s, "purging peer %s because it's been %d secs since we shared anything", tr_atomAddrStr(atom), idleTime);
            return true;
        }
    }

    return false;
}

static int getReconnectIntervalSecs(struct peer_atom const* atom, time_t const now)
{
    auto sec = int{};
    bool const unreachable = (atom->flags2 & MyflagUnreachable) != 0;

    /* if we were recently connected to this peer and transferring piece
     * data, try to reconnect to them sooner rather that later -- we don't
     * want network troubles to get in the way of a good peer. */
    if (!unreachable && now - atom->piece_data_time <= MinimumReconnectIntervalSecs * 2)
    {
        sec = MinimumReconnectIntervalSecs;
    }
    /* otherwise, the interval depends on how many times we've tried
     * and failed to connect to the peer */
    else
    {
        int step = atom->numFails;

        /* penalize peers that were unreachable the last time we tried */
        if (unreachable)
        {
            step += 2;
        }

        switch (step)
        {
        case 0:
            sec = 0;
            break;

        case 1:
            sec = 10;
            break;

        case 2:
            sec = 60 * 2;
            break;

        case 3:
            sec = 60 * 15;
            break;

        case 4:
            sec = 60 * 30;
            break;

        case 5:
            sec = 60 * 60;
            break;

        default:
            sec = 60 * 120;
            break;
        }
    }

    dbgmsg("reconnect interval for %s is %d seconds", tr_atomAddrStr(atom), sec);
    return sec;
}

static void removePeer(tr_peer* peer)
{
    auto* const s = peer->swarm;
    auto const lock = s->manager->unique_lock();

    struct peer_atom* atom = peer->atom;
    TR_ASSERT(atom != nullptr);

    atom->time = tr_time();

    tr_ptrArrayRemoveSortedPointer(&s->peers, peer, peerCompare);
    --s->stats.peerCount;
    --s->stats.peerFromCount[atom->fromFirst];

    TR_ASSERT(s->stats.peerCount == tr_ptrArraySize(&s->peers));
    TR_ASSERT(s->stats.peerFromCount[atom->fromFirst] >= 0);

    delete peer;
}

static void closePeer(tr_peer* peer)
{
    TR_ASSERT(peer != nullptr);
    auto* const s = peer->swarm;
    peer_atom* const atom = peer->atom;

    /* if we transferred piece data, then they might be good peers,
       so reset their `numFails' weight to zero. otherwise we connected
       to them fruitlessly, so mark it as another fail */
    if (atom->piece_data_time != 0)
    {
        tordbg(s, "resetting atom %s numFails to 0", tr_atomAddrStr(atom));
        atom->numFails = 0;
    }
    else
    {
        ++atom->numFails;
        tordbg(s, "incremented atom %s numFails to %d", tr_atomAddrStr(atom), (int)atom->numFails);
    }

    tordbg(s, "removing bad peer %s", tr_atomAddrStr(peer->atom));
    removePeer(peer);
}

static void removeAllPeers(tr_swarm* swarm)
{
    size_t const n = tr_ptrArraySize(&swarm->peers);
    auto** base = (tr_peer**)tr_ptrArrayBase(&swarm->peers);
    for (auto* peer : std::vector<tr_peer*>{ base, base + n })
    {
        removePeer(peer);
    }

    TR_ASSERT(swarm->stats.peerCount == 0);
}

static auto getPeersToClose(tr_swarm* s, time_t const now_sec)
{
    auto peerCount = int{};
    auto** const peers = (tr_peer**)tr_ptrArrayPeek(&s->peers, &peerCount);

    auto peers_to_close = std::vector<tr_peer*>{};
    auto test = [=](auto* peer)
    {
        return shouldPeerBeClosed(s, peer, peerCount, now_sec);
    };
    std::copy_if(peers, peers + peerCount, std::back_inserter(peers_to_close), test);
    return peers_to_close;
}

static void closeBadPeers(tr_swarm* s, time_t const now_sec)
{
    auto const lock = s->manager->unique_lock();

    for (auto* peer : getPeersToClose(s, now_sec))
    {
        closePeer(peer);
    }
}

struct ComparePeerByActivity
{
    int compare(tr_peer const* a, tr_peer const* b) const // <=>
    {
        if (a->doPurge != b->doPurge)
        {
            return a->doPurge ? 1 : -1;
        }

        /* the one to give us data more recently goes first */
        if (a->atom->piece_data_time != b->atom->piece_data_time)
        {
            return a->atom->piece_data_time > b->atom->piece_data_time ? -1 : 1;
        }

        /* the one we connected to most recently goes first */
        if (a->atom->time != b->atom->time)
        {
            return a->atom->time > b->atom->time ? -1 : 1;
        }

        return 0;
    }

    bool operator()(tr_peer const* a, tr_peer const* b) const // less then
    {
        return compare(a, b) < 0;
    }
};

static void enforceTorrentPeerLimit(tr_swarm* s)
{
    // do we have too many peers?
    int n = tr_ptrArraySize(&s->peers);
    int const max = tr_torrentGetPeerLimit(s->tor);
    if (n <= max)
    {
        return;
    }

    // close all but the `max` most active
    auto peers = std::vector<tr_peer*>{};
    peers.reserve(n);
    auto** base = (tr_peer**)tr_ptrArrayBase(&s->peers);
    std::copy_n(base, n, std::back_inserter(peers));
    std::partial_sort(std::begin(peers), std::begin(peers) + max, std::end(peers), ComparePeerByActivity{});
    std::for_each(std::begin(peers) + max, std::end(peers), closePeer);
}

static void enforceSessionPeerLimit(tr_session* session)
{
    // do we have too many peers?
    size_t const n_peers = std::accumulate(
        std::begin(session->torrents),
        std::end(session->torrents),
        size_t{},
        [](size_t sum, tr_torrent* tor) { return sum + tr_ptrArraySize(&tor->swarm->peers); });
    size_t const max = tr_sessionGetPeerLimit(session);
    if (n_peers <= max)
    {
        return;
    }

    // make a list of all the peers
    auto peers = std::vector<tr_peer*>{};
    peers.reserve(n_peers);
    for (auto* tor : session->torrents)
    {
        size_t const n = tr_ptrArraySize(&tor->swarm->peers);
        auto** base = (tr_peer**)tr_ptrArrayBase(&tor->swarm->peers);
        std::copy_n(base, n, std::back_inserter(peers));
    }

    // close all but the `max` most active
    std::partial_sort(std::begin(peers), std::begin(peers) + max, std::end(peers), ComparePeerByActivity{});
    std::for_each(std::begin(peers) + max, std::end(peers), closePeer);
}

static void makeNewPeerConnections(tr_peerMgr* mgr, size_t max);

static void reconnectPulse(evutil_socket_t /*fd*/, short /*what*/, void* vmgr)
{
    auto* mgr = static_cast<tr_peerMgr*>(vmgr);
    time_t const now_sec = tr_time();

    // remove crappy peers
    for (auto* tor : mgr->session->torrents)
    {
        if (!tor->swarm->isRunning)
        {
            removeAllPeers(tor->swarm);
        }
        else
        {
            closeBadPeers(tor->swarm, now_sec);
        }
    }

    // if we're over the per-torrent peer limits, cull some peers
    for (auto* tor : mgr->session->torrents)
    {
        if (tor->isRunning)
        {
            enforceTorrentPeerLimit(tor->swarm);
        }
    }

    // if we're over the per-session peer limits, cull some peers
    enforceSessionPeerLimit(mgr->session);

    // try to make new peer connections
    int const MaxConnectionsPerPulse = (int)(MaxConnectionsPerSecond * (ReconnectPeriodMsec / 1000.0));
    makeNewPeerConnections(mgr, MaxConnectionsPerPulse);
}

/****
*****
*****  BANDWIDTH ALLOCATION
*****
****/

static void pumpAllPeers(tr_peerMgr* mgr)
{
    for (auto* tor : mgr->session->torrents)
    {
        tr_swarm* s = tor->swarm;

        for (int j = 0, n = tr_ptrArraySize(&s->peers); j < n; ++j)
        {
            static_cast<tr_peerMsgs*>(tr_ptrArrayNth(&s->peers, j))->pulse();
        }
    }
}

static void queuePulse(tr_session* session, tr_direction dir)
{
    TR_ASSERT(tr_isSession(session));
    TR_ASSERT(tr_isDirection(dir));

    if (tr_sessionGetQueueEnabled(session, dir))
    {
        auto const n = tr_sessionCountQueueFreeSlots(session, dir);

        for (auto* tor : tr_sessionGetNextQueuedTorrents(session, dir, n))
        {
            tr_torrentStartNow(tor);

            if (tor->queue_started_callback != nullptr)
            {
                (*tor->queue_started_callback)(tor, tor->queue_started_user_data);
            }
        }
    }
}

static void bandwidthPulse(evutil_socket_t /*fd*/, short /*what*/, void* vmgr)
{
    auto* mgr = static_cast<tr_peerMgr*>(vmgr);
    auto const lock = mgr->unique_lock();
    tr_session* session = mgr->session;

    pumpAllPeers(mgr);

    /* allocate bandwidth to the peers */
    session->bandwidth->allocate(TR_UP, BandwidthPeriodMsec);
    session->bandwidth->allocate(TR_DOWN, BandwidthPeriodMsec);

    /* torrent upkeep */
    for (auto* tor : session->torrents)
    {
        /* possibly stop torrents that have seeded enough */
        tr_torrentCheckSeedLimit(tor);

        /* run the completeness check for any torrents that need it */
        if (tor->swarm->needsCompletenessCheck)
        {
            tor->swarm->needsCompletenessCheck = false;
            tor->recheckCompleteness();
        }

        /* stop torrents that are ready to stop, but couldn't be stopped
           earlier during the peer-io callback call chain */
        if (tor->isStopping)
        {
            tr_torrentStop(tor);
        }

        /* update the torrent's stats */
        tor->swarm->stats.activeWebseedCount = countActiveWebseeds(tor->swarm);
    }

    /* pump the queues */
    queuePulse(session, TR_UP);
    queuePulse(session, TR_DOWN);

    reconnectPulse(0, 0, mgr);

    tr_timerAddMsec(mgr->bandwidthTimer, BandwidthPeriodMsec);
}

/***
****
***/

static int compareAtomPtrsByAddress(void const* va, void const* vb)
{
    struct peer_atom const* a = *(struct peer_atom const* const*)va;
    struct peer_atom const* b = *(struct peer_atom const* const*)vb;

    TR_ASSERT(tr_isAtom(a));
    TR_ASSERT(tr_isAtom(b));

    return tr_address_compare(&a->addr, &b->addr);
}

/* best come first, worst go last */
static int compareAtomPtrsByShelfDate(void const* va, void const* vb)
{
    struct peer_atom const* a = *(struct peer_atom const* const*)va;
    struct peer_atom const* b = *(struct peer_atom const* const*)vb;

    TR_ASSERT(tr_isAtom(a));
    TR_ASSERT(tr_isAtom(b));

    int const data_time_cutoff_secs = 60 * 60;
    time_t const tr_now = tr_time();

    /* primary key: the last piece data time *if* it was within the last hour */
    time_t atime = a->piece_data_time;

    if (atime + data_time_cutoff_secs < tr_now)
    {
        atime = 0;
    }

    time_t btime = b->piece_data_time;

    if (btime + data_time_cutoff_secs < tr_now)
    {
        btime = 0;
    }

    if (atime != btime)
    {
        return atime > btime ? -1 : 1;
    }

    /* secondary key: shelf date. */
    if (a->shelf_date != b->shelf_date)
    {
        return a->shelf_date > b->shelf_date ? -1 : 1;
    }

    return 0;
}

static int getMaxAtomCount(tr_torrent const* tor)
{
    return std::min(50, tor->maxConnectedPeers * 3);
}

static void atomPulse(evutil_socket_t /*fd*/, short /*what*/, void* vmgr)
{
    auto* mgr = static_cast<tr_peerMgr*>(vmgr);
    auto const lock = mgr->unique_lock();

    for (auto* tor : mgr->session->torrents)
    {
        tr_swarm* s = tor->swarm;
        int const maxAtomCount = getMaxAtomCount(tor);
        auto atomCount = int{};
        peer_atom** const atoms = (peer_atom**)tr_ptrArrayPeek(&s->pool, &atomCount);

        if (atomCount > maxAtomCount) /* we've got too many atoms... time to prune */
        {
            int keepCount = 0;
            int testCount = 0;
            struct peer_atom** keep = tr_new(struct peer_atom*, atomCount);
            struct peer_atom** test = tr_new(struct peer_atom*, atomCount);

            /* keep the ones that are in use */
            for (int i = 0; i < atomCount; ++i)
            {
                struct peer_atom* atom = atoms[i];

                if (peerIsInUse(s, atom))
                {
                    keep[keepCount++] = atom;
                }
                else
                {
                    test[testCount++] = atom;
                }
            }

            /* if there's room, keep the best of what's left */
            int i = 0;

            if (keepCount < maxAtomCount)
            {
                qsort(test, testCount, sizeof(struct peer_atom*), compareAtomPtrsByShelfDate);

                while (i < testCount && keepCount < maxAtomCount)
                {
                    keep[keepCount++] = test[i++];
                }
            }

            /* free the culled atoms */
            while (i < testCount)
            {
                tr_free(test[i++]);
            }

            /* rebuild Torrent.pool with what's left */
            tr_ptrArrayDestruct(&s->pool, nullptr);
            s->pool = {};
            qsort(keep, keepCount, sizeof(struct peer_atom*), compareAtomPtrsByAddress);

            for (i = 0; i < keepCount; ++i)
            {
                tr_ptrArrayAppend(&s->pool, keep[i]);
            }

            tordbg(s, "max atom count is %d... pruned from %d to %d\n", maxAtomCount, atomCount, keepCount);

            /* cleanup */
            tr_free(test);
            tr_free(keep);
        }
    }

    tr_timerAddMsec(mgr->atomTimer, AtomPeriodMsec);
}

/***
****
****
****
***/

/* is this atom someone that we'd want to initiate a connection to? */
static bool isPeerCandidate(tr_torrent const* tor, struct peer_atom* atom, time_t const now)
{
    /* not if we're both seeds */
    if (tr_torrentIsSeed(tor) && atomIsSeed(atom))
    {
        return false;
    }

    /* not if we've already got a connection to them... */
    if (peerIsInUse(tor->swarm, atom))
    {
        return false;
    }

    /* not if we just tried them already */
    if (now - atom->time < getReconnectIntervalSecs(atom, now))
    {
        return false;
    }

    /* not if they're blocklisted */
    if (isAtomBlocklisted(tor->session, atom))
    {
        return false;
    }

    /* not if they're banned... */
    if ((atom->flags2 & MyflagBanned) != 0)
    {
        return false;
    }

    return true;
}

struct peer_candidate
{
    uint64_t score;
    tr_torrent* tor;
    struct peer_atom* atom;
};

static bool torrentWasRecentlyStarted(tr_torrent const* tor)
{
    return difftime(tr_time(), tor->startDate) < 120;
}

static constexpr uint64_t addValToKey(uint64_t value, int width, uint64_t addme)
{
    value = value << (uint64_t)width;
    value |= addme;
    return value;
}

/* smaller value is better */
static uint64_t getPeerCandidateScore(tr_torrent const* tor, struct peer_atom const* atom, uint8_t salt)
{
    auto i = uint64_t{};
    auto score = uint64_t{};
    bool const failed = atom->lastConnectionAt < atom->lastConnectionAttemptAt;

    /* prefer peers we've connected to, or never tried, over peers we failed to connect to. */
    i = failed ? 1 : 0;
    score = addValToKey(score, 1, i);

    /* prefer the one we attempted least recently (to cycle through all peers) */
    i = atom->lastConnectionAttemptAt;
    score = addValToKey(score, 32, i);

    /* prefer peers belonging to a torrent of a higher priority */
    switch (tr_torrentGetPriority(tor))
    {
    case TR_PRI_HIGH:
        i = 0;
        break;

    case TR_PRI_NORMAL:
        i = 1;
        break;

    case TR_PRI_LOW:
        i = 2;
        break;
    }

    score = addValToKey(score, 4, i);

    /* prefer recently-started torrents */
    i = torrentWasRecentlyStarted(tor) ? 0 : 1;
    score = addValToKey(score, 1, i);

    /* prefer torrents we're downloading with */
    i = tr_torrentIsSeed(tor) ? 1 : 0;
    score = addValToKey(score, 1, i);

    /* prefer peers that are known to be connectible */
    i = (atom->flags & ADDED_F_CONNECTABLE) != 0 ? 0 : 1;
    score = addValToKey(score, 1, i);

    /* prefer peers that we might be able to upload to */
    i = (atom->flags & ADDED_F_SEED_FLAG) == 0 ? 0 : 1;
    score = addValToKey(score, 1, i);

    /* Prefer peers that we got from more trusted sources.
     * lower `fromBest' values indicate more trusted sources */
    score = addValToKey(score, 4, atom->fromBest);

    /* salt */
    score = addValToKey(score, 8, salt);

    return score;
}

static bool calculateAllSeeds(tr_swarm* swarm)
{
    int nAtoms = 0;
    struct peer_atom** atoms = (struct peer_atom**)tr_ptrArrayPeek(&swarm->pool, &nAtoms);

    for (int i = 0; i < nAtoms; ++i)
    {
        if (!atomIsSeed(atoms[i]))
        {
            return false;
        }
    }

    return true;
}

static bool swarmIsAllSeeds(tr_swarm* swarm)
{
    if (swarm->poolIsAllSeedsDirty)
    {
        swarm->poolIsAllSeeds = calculateAllSeeds(swarm);
        swarm->poolIsAllSeedsDirty = false;
    }

    return swarm->poolIsAllSeeds;
}

/** @return an array of all the atoms we might want to connect to */
static std::vector<peer_candidate> getPeerCandidates(tr_session* session, size_t max)
{
    time_t const now = tr_time();
    uint64_t const now_msec = tr_time_msec();
    /* leave 5% of connection slots for incoming connections -- ticket #2609 */
    int const maxCandidates = tr_sessionGetPeerLimit(session) * 0.95;

    /* count how many peers and atoms we've got */
    int atomCount = 0;
    int peerCount = 0;
    for (auto const* tor : session->torrents)
    {
        atomCount += tr_ptrArraySize(&tor->swarm->pool);
        peerCount += tr_ptrArraySize(&tor->swarm->peers);
    }

    /* don't start any new handshakes if we're full up */
    if (maxCandidates <= peerCount)
    {
        return {};
    }

    auto candidates = std::vector<peer_candidate>{};
    candidates.reserve(atomCount);

    /* populate the candidate array */
    for (auto* tor : session->torrents)
    {
        if (!tor->swarm->isRunning)
        {
            continue;
        }

        /* if everyone in the swarm is seeds and pex is disabled because
         * the torrent is private, then don't initiate connections */
        bool const seeding = tr_torrentIsSeed(tor);
        if (seeding && swarmIsAllSeeds(tor->swarm) && tr_torrentIsPrivate(tor))
        {
            continue;
        }

        /* if we've already got enough peers in this torrent... */
        if (tr_torrentGetPeerLimit(tor) <= tr_ptrArraySize(&tor->swarm->peers))
        {
            continue;
        }

        /* if we've already got enough speed in this torrent... */
        if (seeding && isBandwidthMaxedOut(tor->bandwidth, now_msec, TR_UP))
        {
            continue;
        }

        auto nAtoms = int{};
        peer_atom** atoms = (peer_atom**)tr_ptrArrayPeek(&tor->swarm->pool, &nAtoms);

        for (int i = 0; i < nAtoms; ++i)
        {
            struct peer_atom* atom = atoms[i];

            if (isPeerCandidate(tor, atom, now))
            {
                uint8_t const salt = tr_rand_int_weak(1024);
                candidates.push_back({ getPeerCandidateScore(tor, atom, salt), tor, atom });
            }
        }
    }

    // only keep the best `max` candidates
    if (std::size(candidates) > max)
    {
        std::partial_sort(
            std::begin(candidates),
            std::begin(candidates) + max,
            std::end(candidates),
            [](auto const& a, auto const& b) { return a.score < b.score; });
        candidates.resize(max);
    }

    return candidates;
}

static void initiateConnection(tr_peerMgr* mgr, tr_swarm* s, struct peer_atom* atom)
{
    time_t const now = tr_time();
    bool utp = tr_sessionIsUTPEnabled(mgr->session) && !atom->utp_failed;

    if (atom->fromFirst == TR_PEER_FROM_PEX)
    {
        /* PEX has explicit signalling for uTP support.  If an atom
           originally came from PEX and doesn't have the uTP flag, skip the
           uTP connection attempt.  Are we being optimistic here? */
        utp = utp && (atom->flags & ADDED_F_UTP_FLAGS) != 0;
    }

    tordbg(s, "Starting an OUTGOING%s connection with %s", utp ? " µTP" : "", tr_atomAddrStr(atom));

    tr_peerIo* const io = tr_peerIoNewOutgoing(
        mgr->session,
        mgr->session->bandwidth,
        &atom->addr,
        atom->port,
        s->tor->info.hash,
        s->tor->completeness == TR_SEED,
        utp);

    if (io == nullptr)
    {
        tordbg(s, "peerIo not created; marking peer %s as unreachable", tr_atomAddrStr(atom));
        atom->flags2 |= MyflagUnreachable;
        atom->numFails++;
    }
    else
    {
        tr_handshake* handshake = tr_handshakeNew(io, mgr->session->encryptionMode, on_handshake_done, mgr);

        TR_ASSERT(tr_peerIoGetTorrentHash(io));

        tr_peerIoUnref(io); /* balanced by the initial ref in tr_peerIoNewOutgoing() */

        tr_ptrArrayInsertSorted(&s->outgoingHandshakes, handshake, handshakeCompare);
    }

    atom->lastConnectionAttemptAt = now;
    atom->time = now;
}

static void initiateCandidateConnection(tr_peerMgr* mgr, peer_candidate& c)
{
#if 0

    fprintf(stderr, "Starting an OUTGOING connection with %s - [%s] %s, %s\n", tr_atomAddrStr(c->atom),
        tr_torrentName(c->tor), tr_torrentIsPrivate(c->tor) ? "private" : "public",
        tr_torrentIsSeed(c->tor) ? "seed" : "downloader");

#endif

    initiateConnection(mgr, c.tor->swarm, c.atom);
}

static void makeNewPeerConnections(struct tr_peerMgr* mgr, size_t max)
{
    for (auto& candidate : getPeerCandidates(mgr->session, max))
    {
        initiateCandidateConnection(mgr, candidate);
    }
}
