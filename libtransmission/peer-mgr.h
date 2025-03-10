/*
 * This file Copyright (C) 2007-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <inttypes.h> /* uint16_t */

#ifdef _WIN32
#include <winsock2.h> /* struct in_addr */
#endif

#include "net.h" /* tr_address */
#include "peer-common.h"
#include "peer-socket.h"
#include "quark.h"

/**
 * @addtogroup peers Peers
 * @{
 */

class tr_peerIo;
class tr_peerMsgs;
class tr_swarm;
struct UTPSocket;
struct peer_atom;
struct tr_peerMgr;
struct tr_peer_stat;
struct tr_torrent;

/* added_f's bitwise-or'ed flags */
enum
{
    /* true if the peer supports encryption */
    ADDED_F_ENCRYPTION_FLAG = 1,
    /* true if the peer is a seed or partial seed */
    ADDED_F_SEED_FLAG = 2,
    /* true if the peer supports uTP */
    ADDED_F_UTP_FLAGS = 4,
    /* true if the peer has holepunch support */
    ADDED_F_HOLEPUNCH = 8,
    /* true if the peer telling us about this peer
     * initiated the connection (implying that it is connectible) */
    ADDED_F_CONNECTABLE = 16
};

struct tr_pex
{
    tr_address addr;
    tr_port port; /* this field is in network byte order */
    uint8_t flags;
};

constexpr bool tr_isPex(tr_pex const* pex)
{
    return pex && tr_address_is_valid(&pex->addr);
}

tr_address const* tr_peerAddress(tr_peer const*);

int tr_pexCompare(void const* a, void const* b);

tr_peerMgr* tr_peerMgrNew(tr_session* session);

void tr_peerMgrFree(tr_peerMgr* manager);

bool tr_peerMgrPeerIsSeed(tr_torrent const* tor, tr_address const* addr);

void tr_peerMgrSetUtpSupported(tr_torrent* tor, tr_address const* addr);

void tr_peerMgrSetUtpFailed(tr_torrent* tor, tr_address const* addr, bool failed);

std::vector<tr_block_span_t> tr_peerMgrGetNextRequests(tr_torrent* torrent, tr_peer const* peer, size_t numwant);

bool tr_peerMgrDidPeerRequest(tr_torrent const* torrent, tr_peer const* peer, tr_block_index_t block);

void tr_peerMgrClientSentRequests(tr_torrent* torrent, tr_peer* peer, tr_block_span_t span);

size_t tr_peerMgrCountActiveRequestsToPeer(tr_torrent const* torrent, tr_peer const* peer);

void tr_peerMgrAddIncoming(tr_peerMgr* manager, tr_address* addr, tr_port port, struct tr_peer_socket const socket);

tr_pex* tr_peerMgrCompactToPex(
    void const* compact,
    size_t compactLen,
    uint8_t const* added_f,
    size_t added_f_len,
    size_t* setme_pex_count);

tr_pex* tr_peerMgrCompact6ToPex(
    void const* compact,
    size_t compactLen,
    uint8_t const* added_f,
    size_t added_f_len,
    size_t* pexCount);

size_t tr_peerMgrAddPex(tr_torrent* tor, uint8_t from, tr_pex const* pex, size_t n_pex);

void tr_peerMgrSetSwarmIsAllSeeds(tr_torrent* tor);

enum
{
    TR_PEERS_CONNECTED,
    TR_PEERS_INTERESTING
};

int tr_peerMgrGetPeers(
    tr_torrent const* tor,
    tr_pex** setme_pex,
    uint8_t address_type,
    uint8_t peer_list_mode,
    int max_peer_count);

void tr_peerMgrStartTorrent(tr_torrent* tor);

void tr_peerMgrStopTorrent(tr_torrent* tor);

void tr_peerMgrAddTorrent(tr_peerMgr* manager, struct tr_torrent* tor);

void tr_peerMgrRemoveTorrent(tr_torrent* tor);

void tr_peerMgrTorrentAvailability(tr_torrent const* tor, int8_t* tab, unsigned int tabCount);

uint64_t tr_peerMgrGetDesiredAvailable(tr_torrent const* tor);

void tr_peerMgrOnTorrentGotMetainfo(tr_torrent* tor);

void tr_peerMgrOnBlocklistChanged(tr_peerMgr* manager);

struct tr_peer_stat* tr_peerMgrPeerStats(tr_torrent const* tor, int* setmeCount);

double* tr_peerMgrWebSpeeds_KBps(tr_torrent const* tor);

unsigned int tr_peerGetPieceSpeed_Bps(tr_peer const* peer, uint64_t now, tr_direction direction);

void tr_peerMgrClearInterest(tr_torrent* tor);

void tr_peerMgrGotBadPiece(tr_torrent* tor, tr_piece_index_t pieceIndex);

void tr_peerMgrPieceCompleted(tr_torrent* tor, tr_piece_index_t pieceIndex);

/* @} */
