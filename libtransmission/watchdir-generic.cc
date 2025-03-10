/*
 * This file Copyright (C) 2015-2016 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <cerrno>
#include <string>
#include <unordered_set>

#include <event2/event.h>

#define LIBTRANSMISSION_WATCHDIR_MODULE

#include "transmission.h"
#include "log.h"
#include "tr-assert.h"
#include "utils.h"
#include "watchdir.h"
#include "watchdir-common.h"

/***
****
***/

#define log_error(...) \
    (!tr_logLevelIsActive(TR_LOG_ERROR) ? (void)0 : \
                                          tr_logAddMessage(__FILE__, __LINE__, TR_LOG_ERROR, "watchdir:generic", __VA_ARGS__))

/***
****
***/

struct tr_watchdir_generic
{
    tr_watchdir_backend base;

    struct event* event;
    std::unordered_set<std::string> dir_entries;
};

#define BACKEND_UPCAST(b) (reinterpret_cast<tr_watchdir_generic*>(b))

/* Non-static and mutable for unit tests. default to 10 sec. */
auto tr_watchdir_generic_interval = timeval{ 10, 0 };

/***
****
***/

static void tr_watchdir_generic_on_event(evutil_socket_t /*fd*/, short /*type*/, void* context)
{
    auto const handle = static_cast<tr_watchdir_t>(context);
    tr_watchdir_generic* const backend = BACKEND_UPCAST(tr_watchdir_get_backend(handle));

    tr_watchdir_scan(handle, &backend->dir_entries);
}

static void tr_watchdir_generic_free(tr_watchdir_backend* backend_base)
{
    tr_watchdir_generic* const backend = BACKEND_UPCAST(backend_base);

    if (backend == nullptr)
    {
        return;
    }

    TR_ASSERT(backend->base.free_func == &tr_watchdir_generic_free);

    if (backend->event != nullptr)
    {
        event_del(backend->event);
        event_free(backend->event);
    }

    delete backend;
}

tr_watchdir_backend* tr_watchdir_generic_new(tr_watchdir_t handle)
{
    auto* backend = new tr_watchdir_generic{};
    backend->base.free_func = &tr_watchdir_generic_free;

    if ((backend
             ->event = event_new(tr_watchdir_get_event_base(handle), -1, EV_PERSIST, &tr_watchdir_generic_on_event, handle)) ==
        nullptr)
    {
        log_error("Failed to create event: %s", tr_strerror(errno));
        goto FAIL;
    }

    if (event_add(backend->event, &tr_watchdir_generic_interval) == -1)
    {
        log_error("Failed to add event: %s", tr_strerror(errno));
        goto FAIL;
    }

    /* Run initial scan on startup */
    event_active(backend->event, EV_READ, 0);

    return BACKEND_DOWNCAST(backend);

FAIL:
    tr_watchdir_generic_free(BACKEND_DOWNCAST(backend));
    return nullptr;
}
