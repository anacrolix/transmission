/*
 * This file Copyright (C) 2010-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <cerrno> /* errno, EAFNOSUPPORT */
#include <cstring> /* memcpy(), memset() */
#include <vector>

#include <event2/buffer.h>
#include <event2/dns.h>
#include <event2/util.h>

#define LIBTRANSMISSION_ANNOUNCER_MODULE

#include "transmission.h"
#include "announcer.h"
#include "announcer-common.h"
#include "crypto-utils.h" /* tr_rand_buffer() */
#include "log.h"
#include "peer-io.h"
#include "peer-mgr.h" /* tr_peerMgrCompactToPex() */
#include "ptrarray.h"
#include "session.h"
#include "tr-assert.h"
#include "tr-udp.h"
#include "utils.h"

#define dbgmsg(key, ...) tr_logAddDeepNamed(tr_quark_get_string(key), __VA_ARGS__)

/****
*****
****/

static void tau_sockaddr_setport(struct sockaddr* sa, tr_port port)
{
    if (sa->sa_family == AF_INET)
    {
        TR_DISCARD_ALIGN(sa, struct sockaddr_in*)->sin_port = htons(port);
    }
    else if (sa->sa_family == AF_INET6)
    {
        TR_DISCARD_ALIGN(sa, struct sockaddr_in6*)->sin6_port = htons(port);
    }
}

static int tau_sendto(tr_session const* session, struct evutil_addrinfo* ai, tr_port port, void const* buf, size_t buflen)
{
    auto sockfd = tr_socket_t{};

    if (ai->ai_addr->sa_family == AF_INET)
    {
        sockfd = session->udp_socket;
    }
    else if (ai->ai_addr->sa_family == AF_INET6)
    {
        sockfd = session->udp6_socket;
    }
    else
    {
        sockfd = TR_BAD_SOCKET;
    }

    if (sockfd == TR_BAD_SOCKET)
    {
        errno = EAFNOSUPPORT;
        return -1;
    }

    tau_sockaddr_setport(ai->ai_addr, port);
    return sendto(sockfd, static_cast<char const*>(buf), buflen, 0, ai->ai_addr, ai->ai_addrlen);
}

/****
*****
****/

static uint32_t evbuffer_read_ntoh_32(struct evbuffer* buf)
{
    auto val = uint32_t{};
    evbuffer_remove(buf, &val, sizeof(uint32_t));
    return ntohl(val);
}

static uint64_t evbuffer_read_ntoh_64(struct evbuffer* buf)
{
    auto val = uint64_t{};
    evbuffer_remove(buf, &val, sizeof(uint64_t));
    return tr_ntohll(val);
}

/****
*****
****/

using tau_connection_t = uint64_t;

static auto constexpr TauConnectionTtlSecs = int{ 60 };

using tau_transaction_t = uint32_t;

static tau_transaction_t tau_transaction_new(void)
{
    auto tmp = tau_transaction_t{};
    tr_rand_buffer(&tmp, sizeof(tau_transaction_t));
    return tmp;
}

/* used in the "action" field of a request */
enum tau_action_t
{
    TAU_ACTION_CONNECT = 0,
    TAU_ACTION_ANNOUNCE = 1,
    TAU_ACTION_SCRAPE = 2,
    TAU_ACTION_ERROR = 3
};

static bool is_tau_response_message(tau_action_t action, size_t msglen)
{
    if (action == TAU_ACTION_CONNECT)
    {
        return msglen == 16;
    }

    if (action == TAU_ACTION_ANNOUNCE)
    {
        return msglen >= 20;
    }

    if (action == TAU_ACTION_SCRAPE)
    {
        return msglen >= 20;
    }

    if (action == TAU_ACTION_ERROR)
    {
        return msglen >= 8;
    }

    return false;
}

static auto constexpr TauRequestTtl = int{ 60 };

/****
*****
*****  SCRAPE
*****
****/

struct tau_scrape_request
{
    std::vector<uint8_t> payload;

    time_t sent_at;
    time_t created_at;
    tau_transaction_t transaction_id;

    tr_scrape_response response;
    tr_scrape_response_func callback;
    void* user_data;
};

static struct tau_scrape_request* tau_scrape_request_new(
    tr_scrape_request const* in,
    tr_scrape_response_func callback,
    void* user_data)
{
    tau_transaction_t const transaction_id = tau_transaction_new();

    /* build the payload */
    auto* buf = evbuffer_new();
    evbuffer_add_hton_32(buf, TAU_ACTION_SCRAPE);
    evbuffer_add_hton_32(buf, transaction_id);
    for (int i = 0; i < in->info_hash_count; ++i)
    {
        evbuffer_add(buf, std::data(in->info_hash[i]), std::size(in->info_hash[i]));
    }
    auto const* const payload_begin = evbuffer_pullup(buf, -1);
    auto const* const payload_end = payload_begin + evbuffer_get_length(buf);

    /* build the tau_scrape_request */

    auto* req = new tau_scrape_request{};
    req->callback = callback;
    req->created_at = tr_time();
    req->transaction_id = transaction_id;
    req->callback = callback;
    req->user_data = user_data;
    req->response.scrape_url = in->scrape_url;
    req->response.row_count = in->info_hash_count;
    req->payload.assign(payload_begin, payload_end);

    for (int i = 0; i < req->response.row_count; ++i)
    {
        req->response.rows[i].seeders = -1;
        req->response.rows[i].leechers = -1;
        req->response.rows[i].downloads = -1;
        req->response.rows[i].info_hash = in->info_hash[i];
    }

    /* cleanup */
    evbuffer_free(buf);
    return req;
}

static void tau_scrape_request_free(struct tau_scrape_request* req)
{
    delete req;
}

static void tau_scrape_request_finished(struct tau_scrape_request const* request)
{
    if (request->callback != nullptr)
    {
        request->callback(&request->response, request->user_data);
    }
}

static void tau_scrape_request_fail(struct tau_scrape_request* request, bool did_connect, bool did_timeout, char const* errmsg)
{
    request->response.did_connect = did_connect;
    request->response.did_timeout = did_timeout;
    request->response.errmsg = errmsg == nullptr ? "" : errmsg;
    tau_scrape_request_finished(request);
}

static void on_scrape_response(struct tau_scrape_request* request, tau_action_t action, struct evbuffer* buf)
{
    request->response.did_connect = true;
    request->response.did_timeout = false;

    if (action == TAU_ACTION_SCRAPE)
    {
        for (int i = 0; i < request->response.row_count; ++i)
        {
            if (evbuffer_get_length(buf) < sizeof(uint32_t) * 3)
            {
                break;
            }

            struct tr_scrape_response_row& row = request->response.rows[i];
            row.seeders = evbuffer_read_ntoh_32(buf);
            row.downloads = evbuffer_read_ntoh_32(buf);
            row.leechers = evbuffer_read_ntoh_32(buf);
        }

        tau_scrape_request_finished(request);
    }
    else
    {
        size_t const buflen = evbuffer_get_length(buf);
        char* const errmsg = action == TAU_ACTION_ERROR && buflen > 0 ? tr_strndup(evbuffer_pullup(buf, -1), buflen) :
                                                                        tr_strdup(_("Unknown error"));
        tau_scrape_request_fail(request, true, false, errmsg);
        tr_free(errmsg);
    }
}

/****
*****
*****  ANNOUNCE
*****
****/

struct tau_announce_request
{
    std::vector<uint8_t> payload;

    time_t created_at;
    time_t sent_at;
    tau_transaction_t transaction_id;

    tr_announce_response response;
    tr_announce_response_func callback;
    void* user_data;
};

enum tau_announce_event
{
    /* used in the "event" field of an announce request */
    TAU_ANNOUNCE_EVENT_NONE = 0,
    TAU_ANNOUNCE_EVENT_COMPLETED = 1,
    TAU_ANNOUNCE_EVENT_STARTED = 2,
    TAU_ANNOUNCE_EVENT_STOPPED = 3
};

static tau_announce_event get_tau_announce_event(tr_announce_event e)
{
    switch (e)
    {
    case TR_ANNOUNCE_EVENT_COMPLETED:
        return TAU_ANNOUNCE_EVENT_COMPLETED;

    case TR_ANNOUNCE_EVENT_STARTED:
        return TAU_ANNOUNCE_EVENT_STARTED;

    case TR_ANNOUNCE_EVENT_STOPPED:
        return TAU_ANNOUNCE_EVENT_STOPPED;

    default:
        return TAU_ANNOUNCE_EVENT_NONE;
    }
}

static struct tau_announce_request* tau_announce_request_new(
    tr_announce_request const* in,
    tr_announce_response_func callback,
    void* user_data)
{
    tau_transaction_t const transaction_id = tau_transaction_new();

    /* build the payload */
    auto* buf = evbuffer_new();
    evbuffer_add_hton_32(buf, TAU_ACTION_ANNOUNCE);
    evbuffer_add_hton_32(buf, transaction_id);
    evbuffer_add(buf, std::data(in->info_hash), std::size(in->info_hash));
    evbuffer_add(buf, std::data(in->peer_id), std::size(in->peer_id));
    evbuffer_add_hton_64(buf, in->down);
    evbuffer_add_hton_64(buf, in->leftUntilComplete);
    evbuffer_add_hton_64(buf, in->up);
    evbuffer_add_hton_32(buf, get_tau_announce_event(in->event));
    evbuffer_add_hton_32(buf, 0);
    evbuffer_add_hton_32(buf, in->key);
    evbuffer_add_hton_32(buf, in->numwant);
    evbuffer_add_hton_16(buf, in->port);
    auto const* const payload_begin = evbuffer_pullup(buf, -1);
    auto const* const payload_end = payload_begin + evbuffer_get_length(buf);

    /* build the tau_announce_request */
    auto* req = new tau_announce_request{};
    req->created_at = tr_time();
    req->transaction_id = transaction_id;
    req->callback = callback;
    req->user_data = user_data;
    req->payload.assign(payload_begin, payload_end);
    req->response.seeders = -1;
    req->response.leechers = -1;
    req->response.downloads = -1;
    req->response.info_hash = in->info_hash;

    evbuffer_free(buf);
    return req;
}

static void tau_announce_request_free(struct tau_announce_request* req)
{
    tr_free(req->response.tracker_id_str);
    tr_free(req->response.warning);
    tr_free(req->response.errmsg);
    tr_free(req->response.pex6);
    tr_free(req->response.pex);
    delete req;
}

static void tau_announce_request_finished(struct tau_announce_request const* request)
{
    if (request->callback != nullptr)
    {
        request->callback(&request->response, request->user_data);
    }
}

static void tau_announce_request_fail(
    struct tau_announce_request* request,
    bool did_connect,
    bool did_timeout,
    char const* errmsg)
{
    request->response.did_connect = did_connect;
    request->response.did_timeout = did_timeout;
    request->response.errmsg = tr_strdup(errmsg);
    tau_announce_request_finished(request);
}

static void on_announce_response(struct tau_announce_request* request, tau_action_t action, struct evbuffer* buf)
{
    size_t const buflen = evbuffer_get_length(buf);

    request->response.did_connect = true;
    request->response.did_timeout = false;

    if (action == TAU_ACTION_ANNOUNCE && buflen >= 3 * sizeof(uint32_t))
    {
        tr_announce_response* resp = &request->response;
        resp->interval = evbuffer_read_ntoh_32(buf);
        resp->leechers = evbuffer_read_ntoh_32(buf);
        resp->seeders = evbuffer_read_ntoh_32(buf);
        resp->pex = tr_peerMgrCompactToPex(
            evbuffer_pullup(buf, -1),
            evbuffer_get_length(buf),
            nullptr,
            0,
            &request->response.pex_count);
        tau_announce_request_finished(request);
    }
    else
    {
        char* const errmsg = action == TAU_ACTION_ERROR && buflen > 0 ? tr_strndup(evbuffer_pullup(buf, -1), buflen) :
                                                                        tr_strdup(_("Unknown error"));
        tau_announce_request_fail(request, true, false, errmsg);
        tr_free(errmsg);
    }
}

/****
*****
*****  TRACKERS
*****
****/

struct tau_tracker
{
    tr_session* const session;

    tr_quark const key;
    tr_quark const host;
    int const port;

    struct evdns_getaddrinfo_request* dns_request = nullptr;
    struct evutil_addrinfo* addr = nullptr;
    time_t addr_expiration_time = 0;

    time_t connecting_at = 0;
    time_t connection_expiration_time = 0;
    tau_connection_t connection_id = 0;
    tau_transaction_t connection_transaction_id = 0;

    time_t close_at = 0;

    tr_ptrArray announces = {};
    tr_ptrArray scrapes = {};

    tau_tracker(tr_session* session_in, tr_quark key_in, tr_quark host_in, int port_in)
        : session{ session_in }
        , key{ key_in }
        , host{ host_in }
        , port{ port_in }
    {
    }
};

static void tau_tracker_upkeep(struct tau_tracker*);

static void tau_tracker_free(struct tau_tracker* t)
{
    TR_ASSERT(t->dns_request == nullptr);

    if (t->addr != nullptr)
    {
        evutil_freeaddrinfo(t->addr);
    }

    tr_ptrArrayDestruct(&t->announces, (PtrArrayForeachFunc)tau_announce_request_free);
    tr_ptrArrayDestruct(&t->scrapes, (PtrArrayForeachFunc)tau_scrape_request_free);
    delete t;
}

static void tau_tracker_fail_all(struct tau_tracker* tracker, bool did_connect, bool did_timeout, char const* errmsg)
{
    /* fail all the scrapes */
    tr_ptrArray* reqs = &tracker->scrapes;

    for (int i = 0, n = tr_ptrArraySize(reqs); i < n; ++i)
    {
        auto* req = static_cast<struct tau_scrape_request*>(tr_ptrArrayNth(reqs, i));
        tau_scrape_request_fail(req, did_connect, did_timeout, errmsg);
    }

    tr_ptrArrayDestruct(reqs, (PtrArrayForeachFunc)tau_scrape_request_free);
    *reqs = {};

    /* fail all the announces */
    reqs = &tracker->announces;

    for (int i = 0, n = tr_ptrArraySize(reqs); i < n; ++i)
    {
        auto* req = static_cast<struct tau_announce_request*>(tr_ptrArrayNth(reqs, i));
        tau_announce_request_fail(req, did_connect, did_timeout, errmsg);
    }

    tr_ptrArrayDestruct(reqs, (PtrArrayForeachFunc)tau_announce_request_free);
    *reqs = {};
}

static void tau_tracker_on_dns(int errcode, struct evutil_addrinfo* addr, void* vtracker)
{
    auto* tracker = static_cast<struct tau_tracker*>(vtracker);

    tracker->dns_request = nullptr;

    if (errcode != 0)
    {
        char* errmsg = tr_strdup_printf(_("DNS Lookup failed: %s"), evutil_gai_strerror(errcode));
        dbgmsg(tracker->key, "%s", errmsg);
        tau_tracker_fail_all(tracker, false, false, errmsg);
        tr_free(errmsg);
    }
    else
    {
        dbgmsg(tracker->key, "DNS lookup succeeded");
        tracker->addr = addr;
        tracker->addr_expiration_time = tr_time() + 60 * 60; /* one hour */
        tau_tracker_upkeep(tracker);
    }
}

static void tau_tracker_send_request(struct tau_tracker* tracker, void const* payload, size_t payload_len)
{
    struct evbuffer* buf = evbuffer_new();
    dbgmsg(tracker->key, "sending request w/connection id %" PRIu64 "\n", tracker->connection_id);
    evbuffer_add_hton_64(buf, tracker->connection_id);
    evbuffer_add_reference(buf, payload, payload_len, nullptr, nullptr);
    (void)tau_sendto(tracker->session, tracker->addr, tracker->port, evbuffer_pullup(buf, -1), evbuffer_get_length(buf));
    evbuffer_free(buf);
}

static void tau_tracker_send_reqs(struct tau_tracker* tracker)
{
    TR_ASSERT(tracker->dns_request == nullptr);
    TR_ASSERT(tracker->connecting_at == 0);
    TR_ASSERT(tracker->addr != nullptr);

    time_t const now = tr_time();

    TR_ASSERT(tracker->connection_expiration_time > now);

    tr_ptrArray* reqs = &tracker->announces;

    for (int i = 0, n = tr_ptrArraySize(reqs); i < n; ++i)
    {
        auto* req = static_cast<struct tau_announce_request*>(tr_ptrArrayNth(reqs, i));

        if (req->sent_at == 0)
        {
            dbgmsg(tracker->key, "sending announce req %p", (void*)req);
            req->sent_at = now;
            tau_tracker_send_request(tracker, std::data(req->payload), std::size(req->payload));

            if (req->callback == nullptr)
            {
                tau_announce_request_free(req);
                tr_ptrArrayRemove(reqs, i);
                --i;
                --n;
            }
        }
    }

    reqs = &tracker->scrapes;

    for (int i = 0, n = tr_ptrArraySize(reqs); i < n; ++i)
    {
        auto* req = static_cast<struct tau_scrape_request*>(tr_ptrArrayNth(reqs, i));

        if (req->sent_at == 0)
        {
            dbgmsg(tracker->key, "sending scrape req %p", (void*)req);
            req->sent_at = now;
            tau_tracker_send_request(tracker, std::data(req->payload), std::size(req->payload));

            if (req->callback == nullptr)
            {
                tau_scrape_request_free(req);
                tr_ptrArrayRemove(reqs, i);
                --i;
                --n;
            }
        }
    }
}

static void on_tracker_connection_response(struct tau_tracker* tracker, tau_action_t action, struct evbuffer* buf)
{
    time_t const now = tr_time();

    tracker->connecting_at = 0;
    tracker->connection_transaction_id = 0;

    if (action == TAU_ACTION_CONNECT)
    {
        tracker->connection_id = evbuffer_read_ntoh_64(buf);
        tracker->connection_expiration_time = now + TauConnectionTtlSecs;
        dbgmsg(tracker->key, "Got a new connection ID from tracker: %" PRIu64, tracker->connection_id);
    }
    else
    {
        size_t const buflen = buf != nullptr ? evbuffer_get_length(buf) : 0;

        char* const errmsg = action == TAU_ACTION_ERROR && buflen > 0 ? tr_strndup(evbuffer_pullup(buf, -1), buflen) :
                                                                        tr_strdup(_("Connection failed"));

        dbgmsg(tracker->key, "%s", errmsg);
        tau_tracker_fail_all(tracker, true, false, errmsg);
        tr_free(errmsg);
    }

    tau_tracker_upkeep(tracker);
}

static void tau_tracker_timeout_reqs(struct tau_tracker* tracker)
{
    time_t const now = time(nullptr);
    bool const cancel_all = tracker->close_at != 0 && (tracker->close_at <= now);

    if (tracker->connecting_at != 0 && tracker->connecting_at + TauRequestTtl < now)
    {
        on_tracker_connection_response(tracker, TAU_ACTION_ERROR, nullptr);
    }

    tr_ptrArray* reqs = &tracker->announces;

    for (int i = 0, n = tr_ptrArraySize(reqs); i < n; ++i)
    {
        auto* req = static_cast<struct tau_announce_request*>(tr_ptrArrayNth(reqs, i));

        if (cancel_all || req->created_at + TauRequestTtl < now)
        {
            dbgmsg(tracker->key, "timeout announce req %p", (void*)req);
            tau_announce_request_fail(req, false, true, nullptr);
            tau_announce_request_free(req);
            tr_ptrArrayRemove(reqs, i);
            --i;
            --n;
        }
    }

    reqs = &tracker->scrapes;

    for (int i = 0, n = tr_ptrArraySize(reqs); i < n; ++i)
    {
        auto* const req = static_cast<struct tau_scrape_request*>(tr_ptrArrayNth(reqs, i));

        if (cancel_all || req->created_at + TauRequestTtl < now)
        {
            dbgmsg(tracker->key, "timeout scrape req %p", (void*)req);
            tau_scrape_request_fail(req, false, true, nullptr);
            tau_scrape_request_free(req);
            tr_ptrArrayRemove(reqs, i);
            --i;
            --n;
        }
    }
}

static bool tau_tracker_is_idle(struct tau_tracker const* tracker)
{
    return tr_ptrArrayEmpty(&tracker->announces) && tr_ptrArrayEmpty(&tracker->scrapes) && tracker->dns_request == nullptr;
}

static void tau_tracker_upkeep_ex(struct tau_tracker* tracker, bool timeout_reqs)
{
    time_t const now = tr_time();
    bool const closing = tracker->close_at != 0;

    /* if the address info is too old, expire it */
    if (tracker->addr != nullptr && (closing || tracker->addr_expiration_time <= now))
    {
        dbgmsg(tracker->host, "Expiring old DNS result");
        evutil_freeaddrinfo(tracker->addr);
        tracker->addr = nullptr;
    }

    /* are there any requests pending? */
    if (tau_tracker_is_idle(tracker))
    {
        return;
    }

    /* if we don't have an address yet, try & get one now. */
    if (!closing && tracker->addr == nullptr && tracker->dns_request == nullptr)
    {
        struct evutil_addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        dbgmsg(tracker->host, "Trying a new DNS lookup");
        tracker->dns_request = evdns_getaddrinfo(
            tracker->session->evdns_base,
            tr_quark_get_string(tracker->host),
            nullptr,
            &hints,
            tau_tracker_on_dns,
            tracker);
        return;
    }

    dbgmsg(
        tracker->key,
        "addr %p -- connected %d (%zu %zu) -- connecting_at %zu",
        (void*)tracker->addr,
        (int)(tracker->connection_expiration_time > now),
        (size_t)tracker->connection_expiration_time,
        (size_t)now,
        (size_t)tracker->connecting_at);

    /* also need a valid connection ID... */
    if (tracker->addr != nullptr && tracker->connection_expiration_time <= now && tracker->connecting_at == 0)
    {
        struct evbuffer* buf = evbuffer_new();
        tracker->connecting_at = now;
        tracker->connection_transaction_id = tau_transaction_new();
        dbgmsg(tracker->key, "Trying to connect. Transaction ID is %u", tracker->connection_transaction_id);
        evbuffer_add_hton_64(buf, 0x41727101980LL);
        evbuffer_add_hton_32(buf, TAU_ACTION_CONNECT);
        evbuffer_add_hton_32(buf, tracker->connection_transaction_id);
        (void)tau_sendto(tracker->session, tracker->addr, tracker->port, evbuffer_pullup(buf, -1), evbuffer_get_length(buf));
        evbuffer_free(buf);
        return;
    }

    if (timeout_reqs)
    {
        tau_tracker_timeout_reqs(tracker);
    }

    if (tracker->addr != nullptr && tracker->connection_expiration_time > now)
    {
        tau_tracker_send_reqs(tracker);
    }
}

static void tau_tracker_upkeep(struct tau_tracker* tracker)
{
    tau_tracker_upkeep_ex(tracker, true);
}

/****
*****
*****  SESSION
*****
****/

struct tr_announcer_udp
{
    /* tau_tracker */
    tr_ptrArray trackers;

    tr_session* session;
};

static struct tr_announcer_udp* announcer_udp_get(tr_session* session)
{
    if (session->announcer_udp != nullptr)
    {
        return session->announcer_udp;
    }

    auto* const tau = tr_new0(tr_announcer_udp, 1);
    tau->trackers = {};
    tau->session = session;
    session->announcer_udp = tau;
    return tau;
}

/* Finds the tau_tracker struct that corresponds to this url.
   If it doesn't exist yet, create one. */
static tau_tracker* tau_session_get_tracker(tr_announcer_udp* tau, tr_quark announce_url)
{
    // build a lookup key for this tracker
    auto const announce_sv = tr_quark_get_string_view(announce_url);
    auto parsed = tr_urlParseTracker(announce_sv);
    TR_ASSERT(parsed);
    if (!parsed)
    {
        return nullptr;
    }

    // see if we already have it
    // TODO: replace tr_ptrArray
    auto const key = tr_announcerGetKey(*parsed);
    for (int i = 0, n = tr_ptrArraySize(&tau->trackers); i < n; ++i)
    {
        auto* tmp = static_cast<struct tau_tracker*>(tr_ptrArrayNth(&tau->trackers, i));
        if (tmp->key == key)
        {
            return tmp;
        }
    }

    // we don't have it -- build a new one
    auto* const tracker = new tau_tracker{ tau->session, key, tr_quark_new(parsed->host), parsed->port };
    tr_ptrArrayAppend(&tau->trackers, tracker);
    dbgmsg(tracker->key, "New tau_tracker created");
    return tracker;
}

/****
*****
*****  PUBLIC API
*****
****/

void tr_tracker_udp_upkeep(tr_session* session)
{
    struct tr_announcer_udp* tau = session->announcer_udp;

    if (tau != nullptr)
    {
        tr_ptrArrayForeach(&tau->trackers, (PtrArrayForeachFunc)tau_tracker_upkeep);
    }
}

bool tr_tracker_udp_is_idle(tr_session const* session)
{
    struct tr_announcer_udp* tau = session->announcer_udp;

    if (tau != nullptr)
    {
        for (int i = 0, n = tr_ptrArraySize(&tau->trackers); i < n; ++i)
        {
            auto const* tracker = static_cast<struct tau_tracker const*>(tr_ptrArrayNth(&tau->trackers, i));
            if (!tau_tracker_is_idle(tracker))
            {
                return false;
            }
        }
    }

    return true;
}

/* drop dead now. */
void tr_tracker_udp_close(tr_session* session)
{
    struct tr_announcer_udp* tau = session->announcer_udp;

    if (tau != nullptr)
    {
        session->announcer_udp = nullptr;
        tr_ptrArrayDestruct(&tau->trackers, (PtrArrayForeachFunc)tau_tracker_free);
        tr_free(tau);
    }
}

/* start shutting down.
   This doesn't destroy everything if there are requests,
   but sets a deadline on how much longer to wait for the remaining ones */
void tr_tracker_udp_start_shutdown(tr_session* session)
{
    time_t const now = time(nullptr);
    struct tr_announcer_udp* tau = session->announcer_udp;

    if (tau != nullptr)
    {
        for (int i = 0, n = tr_ptrArraySize(&tau->trackers); i < n; ++i)
        {
            auto* tracker = static_cast<struct tau_tracker*>(tr_ptrArrayNth(&tau->trackers, i));

            if (tracker->dns_request != nullptr)
            {
                evdns_getaddrinfo_cancel(tracker->dns_request);
            }

            tracker->close_at = now + 3;
            tau_tracker_upkeep(tracker);
        }
    }
}

/* @brief process an incoming udp message if it's a tracker response.
 * @return true if msg was a tracker response; false otherwise */
bool tau_handle_message(tr_session* session, uint8_t const* msg, size_t msglen)
{
    if (session == nullptr || session->announcer_udp == nullptr)
    {
        return false;
    }

    if (msglen < sizeof(uint32_t) * 2)
    {
        return false;
    }

    /* extract the action_id and see if it makes sense */
    struct evbuffer* const buf = evbuffer_new();
    evbuffer_add_reference(buf, msg, msglen, nullptr, nullptr);
    auto const action_id = tau_action_t(evbuffer_read_ntoh_32(buf));

    if (!is_tau_response_message(action_id, msglen))
    {
        evbuffer_free(buf);
        return false;
    }

    /* extract the transaction_id and look for a match */
    struct tr_announcer_udp* const tau = session->announcer_udp;
    tau_transaction_t const transaction_id = evbuffer_read_ntoh_32(buf);

    for (int i = 0, n = tr_ptrArraySize(&tau->trackers); i < n; ++i)
    {
        auto* tracker = static_cast<struct tau_tracker*>(tr_ptrArrayNth(&tau->trackers, i));

        /* is it a connection response? */
        if (tracker->connecting_at != 0 && transaction_id == tracker->connection_transaction_id)
        {
            dbgmsg(tracker->key, "%" PRIu32 " is my connection request!", transaction_id);
            on_tracker_connection_response(tracker, action_id, buf);
            evbuffer_free(buf);
            return true;
        }

        /* is it a response to one of this tracker's announces? */
        tr_ptrArray* reqs = &tracker->announces;

        for (int j = 0, jn = tr_ptrArraySize(reqs); j < jn; ++j)
        {
            auto* req = static_cast<struct tau_announce_request*>(tr_ptrArrayNth(reqs, j));

            if (req->sent_at != 0 && transaction_id == req->transaction_id)
            {
                dbgmsg(tracker->key, "%" PRIu32 " is an announce request!", transaction_id);
                tr_ptrArrayRemove(reqs, j);
                on_announce_response(req, action_id, buf);
                tau_announce_request_free(req);
                evbuffer_free(buf);
                return true;
            }
        }

        /* is it a response to one of this tracker's scrapes? */
        reqs = &tracker->scrapes;

        for (int j = 0, jn = tr_ptrArraySize(reqs); j < jn; ++j)
        {
            auto* const req = static_cast<struct tau_scrape_request*>(tr_ptrArrayNth(reqs, j));

            if (req->sent_at != 0 && transaction_id == req->transaction_id)
            {
                dbgmsg(tracker->key, "%" PRIu32 " is a scrape request!", transaction_id);
                tr_ptrArrayRemove(reqs, j);
                on_scrape_response(req, action_id, buf);
                tau_scrape_request_free(req);
                evbuffer_free(buf);
                return true;
            }
        }
    }

    /* no match... */
    evbuffer_free(buf);
    return false;
}

void tr_tracker_udp_announce(
    tr_session* session,
    tr_announce_request const* request,
    tr_announce_response_func response_func,
    void* user_data)
{
    tr_announcer_udp* tau = announcer_udp_get(session);
    tau_tracker* tracker = tau_session_get_tracker(tau, request->announce_url);
    if (tracker == nullptr)
    {
        return;
    }

    tau_announce_request* r = tau_announce_request_new(request, response_func, user_data);
    tr_ptrArrayAppend(&tracker->announces, r);
    tau_tracker_upkeep_ex(tracker, false);
}

void tr_tracker_udp_scrape(
    tr_session* session,
    tr_scrape_request const* request,
    tr_scrape_response_func response_func,
    void* user_data)
{
    tr_announcer_udp* tau = announcer_udp_get(session);
    tau_tracker* tracker = tau_session_get_tracker(tau, request->scrape_url);
    if (tracker == nullptr)
    {
        return;
    }

    tau_scrape_request* r = tau_scrape_request_new(request, response_func, user_data);
    tr_ptrArrayAppend(&tracker->scrapes, r);
    tau_tracker_upkeep_ex(tracker, false);
}
