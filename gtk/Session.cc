/******************************************************************************
 * Copyright (c) Transmission authors and contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#include <algorithm>
#include <functional>
#include <map>
#include <math.h> /* pow() */
#include <string.h> /* strlen */

#include <glibmm/i18n.h>

#include <event2/buffer.h>

#include <libtransmission/transmission.h>
#include <libtransmission/log.h>
#include <libtransmission/rpcimpl.h>
#include <libtransmission/tr-assert.h>
#include <libtransmission/utils.h> /* tr_free */
#include <libtransmission/variant.h>

#include "Actions.h"
#include "Notify.h"
#include "Prefs.h"
#include "PrefsDialog.h"
#include "Session.h"
#include "Utils.h"

namespace
{

class ScopedModelSortBlocker
{
public:
    ScopedModelSortBlocker(Gtk::TreeSortable& model)
        : model_(model)
    {
        model_.get_sort_column_id(sort_column_id_, sort_type_);
        model_.set_sort_column(Gtk::TreeSortable::DEFAULT_SORT_COLUMN_ID, Gtk::SORT_ASCENDING);
    }

    ~ScopedModelSortBlocker()
    {
        model_.set_sort_column(sort_column_id_, sort_type_);
    }

private:
    Gtk::TreeSortable& model_;
    int sort_column_id_ = -1;
    Gtk::SortType sort_type_ = Gtk::SORT_ASCENDING;
};

} // namespace

class Session::Impl
{
public:
    Impl(Session& core, tr_session* session);

    tr_session* close();

    Glib::RefPtr<Gtk::ListStore> get_raw_model() const;
    Glib::RefPtr<Gtk::TreeModelSort> get_model() const;
    tr_session* get_session() const;

    size_t get_active_torrent_count() const;

    void update();
    void torrents_added();

    void add_files(std::vector<Glib::RefPtr<Gio::File>> const& files, bool do_start, bool do_prompt, bool do_notify);
    int add_ctor(tr_ctor* ctor, bool do_prompt, bool do_notify);
    void add_torrent(tr_torrent* tor, bool do_notify);
    bool add_from_url(Glib::ustring const& uri);

    void send_rpc_request(tr_variant const* request, int64_t tag, std::function<void(tr_variant*)> const& response_func);

    void commit_prefs_change(tr_quark key);

public:
    sigc::signal<void(ErrorCode, Glib::ustring const&)> signal_add_error;
    sigc::signal<void(tr_ctor*)> signal_add_prompt;
    sigc::signal<void(int)> signal_blocklist_updated;
    sigc::signal<void(bool)> signal_busy;
    sigc::signal<void(tr_quark)> signal_prefs_changed;
    sigc::signal<void(bool)> signal_port_tested;

private:
    Glib::RefPtr<Session> get_core_ptr() const;

    bool is_busy();
    void add_to_busy(int addMe);
    void inc_busy();
    void dec_busy();

    bool add_file(Glib::RefPtr<Gio::File> const& file, bool do_start, bool do_prompt, bool do_notify);
    void add_file_async_callback(
        Glib::RefPtr<Gio::File> const& file,
        Glib::RefPtr<Gio::AsyncResult>& result,
        tr_ctor* ctor,
        bool do_prompt,
        bool do_notify);

    tr_torrent* create_new_torrent(tr_ctor* ctor);

    void set_sort_mode(std::string const& mode, bool is_reversed);

    void maybe_inhibit_hibernation();
    void set_hibernation_allowed(bool allowed);

    void watchdir_update();
    void watchdir_scan();
    void watchdir_monitor_file(Glib::RefPtr<Gio::File> const& file);
    bool watchdir_idle();
    void on_file_changed_in_watchdir(
        Glib::RefPtr<Gio::File> const& file,
        Glib::RefPtr<Gio::File> const& other_type,
        Gio::FileMonitorEvent event_type);

    void on_pref_changed(tr_quark key);

    void on_torrent_completeness_changed(tr_torrent* tor, tr_completeness completeness, bool was_running);
    void on_torrent_metadata_changed(tr_torrent* tor);

private:
    Session& core_;

    Glib::RefPtr<Gio::FileMonitor> monitor_;
    sigc::connection monitor_tag_;
    Glib::RefPtr<Gio::File> monitor_dir_;
    std::vector<Glib::RefPtr<Gio::File>> monitor_files_;
    sigc::connection monitor_idle_tag_;

    bool adding_from_watch_dir_ = false;
    bool inhibit_allowed_ = false;
    bool have_inhibit_cookie_ = false;
    bool dbus_error_ = false;
    guint inhibit_cookie_ = 0;
    gint busy_count_ = 0;
    Glib::RefPtr<Gtk::ListStore> raw_model_;
    Glib::RefPtr<Gtk::TreeModelSort> sorted_model_;
    tr_session* session_ = nullptr;
};

TorrentModelColumns::TorrentModelColumns()
{
    add(name_collated);
    add(torrent);
    add(torrent_id);
    add(speed_up);
    add(speed_down);
    add(active_peers_up);
    add(active_peers_down);
    add(recheck_progress);
    add(active);
    add(activity);
    add(finished);
    add(priority);
    add(queue_position);
    add(trackers);
    add(error);
    add(active_peer_count);
}

TorrentModelColumns const torrent_cols;

Glib::RefPtr<Session> Session::Impl::get_core_ptr() const
{
    core_.reference();
    return Glib::RefPtr<Session>(&core_);
}

/***
****
***/

Glib::RefPtr<Gtk::ListStore> Session::Impl::get_raw_model() const
{
    return raw_model_;
}

Glib::RefPtr<Gtk::TreeModel> Session::get_model() const
{
    return impl_->get_model();
}

Glib::RefPtr<Gtk::TreeModelSort> Session::Impl::get_model() const
{
    return sorted_model_;
}

tr_session* Session::get_session() const
{
    return impl_->get_session();
}

tr_session* Session::Impl::get_session() const
{
    return session_;
}

/***
****  BUSY
***/

bool Session::Impl::is_busy()
{
    return busy_count_ > 0;
}

void Session::Impl::add_to_busy(int addMe)
{
    bool const wasBusy = is_busy();

    busy_count_ += addMe;

    if (wasBusy != is_busy())
    {
        signal_busy.emit(is_busy());
    }
}

void Session::Impl::inc_busy()
{
    add_to_busy(1);
}

void Session::Impl::dec_busy()
{
    add_to_busy(-1);
}

/***
****
****  SORTING THE MODEL
****
***/

namespace
{

bool is_valid_eta(int t)
{
    return t != TR_ETA_NOT_AVAIL && t != TR_ETA_UNKNOWN;
}

int compare_eta(int a, int b)
{
    int ret;

    bool const a_valid = is_valid_eta(a);
    bool const b_valid = is_valid_eta(b);

    if (!a_valid && !b_valid)
    {
        ret = 0;
    }
    else if (!a_valid)
    {
        ret = -1;
    }
    else if (!b_valid)
    {
        ret = 1;
    }
    else
    {
        ret = a < b ? 1 : -1;
    }

    return ret;
}

int compare_double(double a, double b)
{
    int ret;

    if (a < b)
    {
        ret = -1;
    }
    else if (a > b)
    {
        ret = 1;
    }
    else
    {
        ret = 0;
    }

    return ret;
}

int compare_uint64(uint64_t a, uint64_t b)
{
    int ret;

    if (a < b)
    {
        ret = -1;
    }
    else if (a > b)
    {
        ret = 1;
    }
    else
    {
        ret = 0;
    }

    return ret;
}

int compare_int(int a, int b)
{
    int ret;

    if (a < b)
    {
        ret = -1;
    }
    else if (a > b)
    {
        ret = 1;
    }
    else
    {
        ret = 0;
    }

    return ret;
}

int compare_ratio(double a, double b)
{
    int ret;

    if ((int)a == TR_RATIO_INF && (int)b == TR_RATIO_INF)
    {
        ret = 0;
    }
    else if ((int)a == TR_RATIO_INF)
    {
        ret = 1;
    }
    else if ((int)b == TR_RATIO_INF)
    {
        ret = -1;
    }
    else
    {
        ret = compare_double(a, b);
    }

    return ret;
}

int compare_time(time_t a, time_t b)
{
    int ret;

    if (a < b)
    {
        ret = -1;
    }
    else if (a > b)
    {
        ret = 1;
    }
    else
    {
        ret = 0;
    }

    return ret;
}

int compare_by_name(Gtk::TreeModel::iterator const& a, Gtk::TreeModel::iterator const& b)
{
    return a->get_value(torrent_cols.name_collated).compare(b->get_value(torrent_cols.name_collated));
}

int compare_by_queue(Gtk::TreeModel::iterator const& a, Gtk::TreeModel::iterator const& b)
{
    auto const* const sa = tr_torrentStatCached(static_cast<tr_torrent*>(a->get_value(torrent_cols.torrent)));
    auto const* const sb = tr_torrentStatCached(static_cast<tr_torrent*>(b->get_value(torrent_cols.torrent)));

    return sb->queuePosition - sa->queuePosition;
}

int compare_by_ratio(Gtk::TreeModel::iterator const& a, Gtk::TreeModel::iterator const& b)
{
    int ret = 0;

    auto const* const sa = tr_torrentStatCached(static_cast<tr_torrent*>(a->get_value(torrent_cols.torrent)));
    auto const* const sb = tr_torrentStatCached(static_cast<tr_torrent*>(b->get_value(torrent_cols.torrent)));

    if (ret == 0)
    {
        ret = compare_ratio(sa->ratio, sb->ratio);
    }

    if (ret == 0)
    {
        ret = compare_by_queue(a, b);
    }

    return ret;
}

int compare_by_activity(Gtk::TreeModel::iterator const& a, Gtk::TreeModel::iterator const& b)
{
    int ret = 0;

    auto* const ta = static_cast<tr_torrent*>(a->get_value(torrent_cols.torrent));
    auto* const tb = static_cast<tr_torrent*>(b->get_value(torrent_cols.torrent));
    auto const aUp = a->get_value(torrent_cols.speed_up);
    auto const aDown = a->get_value(torrent_cols.speed_down);
    auto const bUp = b->get_value(torrent_cols.speed_up);
    auto const bDown = b->get_value(torrent_cols.speed_down);

    ret = compare_double(aUp + aDown, bUp + bDown);

    if (ret == 0)
    {
        auto const* const sa = tr_torrentStatCached(ta);
        auto const* const sb = tr_torrentStatCached(tb);
        ret = compare_uint64(sa->peersSendingToUs + sa->peersGettingFromUs, sb->peersSendingToUs + sb->peersGettingFromUs);
    }

    if (ret == 0)
    {
        ret = compare_by_queue(a, b);
    }

    return ret;
}

int compare_by_age(Gtk::TreeModel::iterator const& a, Gtk::TreeModel::iterator const& b)
{
    int ret = 0;

    auto* const ta = static_cast<tr_torrent*>(a->get_value(torrent_cols.torrent));
    auto* const tb = static_cast<tr_torrent*>(b->get_value(torrent_cols.torrent));

    if (ret == 0)
    {
        ret = compare_time(tr_torrentStatCached(ta)->addedDate, tr_torrentStatCached(tb)->addedDate);
    }

    if (ret == 0)
    {
        ret = compare_by_name(a, b);
    }

    return ret;
}

int compare_by_size(Gtk::TreeModel::iterator const& a, Gtk::TreeModel::iterator const& b)
{
    int ret = 0;

    auto const* const ia = tr_torrentInfo(static_cast<tr_torrent*>(a->get_value(torrent_cols.torrent)));
    auto const* const ib = tr_torrentInfo(static_cast<tr_torrent*>(b->get_value(torrent_cols.torrent)));

    if (ret == 0)
    {
        ret = compare_uint64(ia->totalSize, ib->totalSize);
    }

    if (ret == 0)
    {
        ret = compare_by_name(a, b);
    }

    return ret;
}

int compare_by_progress(Gtk::TreeModel::iterator const& a, Gtk::TreeModel::iterator const& b)
{
    int ret = 0;

    auto const* const sa = tr_torrentStatCached(static_cast<tr_torrent*>(a->get_value(torrent_cols.torrent)));
    auto const* const sb = tr_torrentStatCached(static_cast<tr_torrent*>(b->get_value(torrent_cols.torrent)));

    if (ret == 0)
    {
        ret = compare_double(sa->percentComplete, sb->percentComplete);
    }

    if (ret == 0)
    {
        ret = compare_double(sa->seedRatioPercentDone, sb->seedRatioPercentDone);
    }

    if (ret == 0)
    {
        ret = compare_by_ratio(a, b);
    }

    return ret;
}

int compare_by_eta(Gtk::TreeModel::iterator const& a, Gtk::TreeModel::iterator const& b)
{
    int ret = 0;

    auto const* const sa = tr_torrentStatCached(static_cast<tr_torrent*>(a->get_value(torrent_cols.torrent)));
    auto const* const sb = tr_torrentStatCached(static_cast<tr_torrent*>(b->get_value(torrent_cols.torrent)));

    if (ret == 0)
    {
        ret = compare_eta(sa->eta, sb->eta);
    }

    if (ret == 0)
    {
        ret = compare_by_name(a, b);
    }

    return ret;
}

int compare_by_state(Gtk::TreeModel::iterator const& a, Gtk::TreeModel::iterator const& b)
{
    int ret = 0;

    auto const sa = a->get_value(torrent_cols.activity);
    auto const sb = b->get_value(torrent_cols.activity);

    if (ret == 0)
    {
        ret = compare_int(sa, sb);
    }

    if (ret == 0)
    {
        ret = compare_by_queue(a, b);
    }

    return ret;
}

} // namespace

void Session::Impl::set_sort_mode(std::string const& mode, bool is_reversed)
{
    auto const& col = torrent_cols.torrent;
    Gtk::TreeSortable::SlotCompare sort_func;
    auto type = is_reversed ? Gtk::SORT_ASCENDING : Gtk::SORT_DESCENDING;
    auto const sortable = get_model();

    if (mode == "sort-by-activity")
    {
        sort_func = &compare_by_activity;
    }
    else if (mode == "sort-by-age")
    {
        sort_func = &compare_by_age;
    }
    else if (mode == "sort-by-progress")
    {
        sort_func = &compare_by_progress;
    }
    else if (mode == "sort-by-queue")
    {
        sort_func = &compare_by_queue;
    }
    else if (mode == "sort-by-time-left")
    {
        sort_func = &compare_by_eta;
    }
    else if (mode == "sort-by-ratio")
    {
        sort_func = &compare_by_ratio;
    }
    else if (mode == "sort-by-state")
    {
        sort_func = &compare_by_state;
    }
    else if (mode == "sort-by-size")
    {
        sort_func = &compare_by_size;
    }
    else
    {
        sort_func = &compare_by_name;
        type = is_reversed ? Gtk::SORT_DESCENDING : Gtk::SORT_ASCENDING;
    }

    sortable->set_sort_func(col, sort_func);
    sortable->set_sort_column(col, type);
}

/***
****
****  WATCHDIR
****
***/

namespace
{

time_t get_file_mtime(Glib::RefPtr<Gio::File> const& file)
{
    auto const info = file->query_info(G_FILE_ATTRIBUTE_TIME_MODIFIED);
    return info != nullptr ? info->get_attribute_uint64(G_FILE_ATTRIBUTE_TIME_MODIFIED) : 0;
}

void rename_torrent(Glib::RefPtr<Gio::File> const& file)
{
    auto const info = file->query_info(G_FILE_ATTRIBUTE_STANDARD_EDIT_NAME);

    if (info != nullptr)
    {
        auto const old_name = info->get_attribute_as_string(G_FILE_ATTRIBUTE_STANDARD_EDIT_NAME);
        auto const new_name = gtr_sprintf("%s.added", old_name);

        try
        {
            file->set_display_name(new_name);
        }
        catch (Glib::Error const& e)
        {
            g_message("Unable to rename \"%s\" as \"%s\": %s", old_name.c_str(), new_name.c_str(), e.what().c_str());
        }
    }
}

} // namespace

bool Session::Impl::watchdir_idle()
{
    std::vector<Glib::RefPtr<Gio::File>> changing;
    std::vector<Glib::RefPtr<Gio::File>> unchanging;
    time_t const now = tr_time();

    /* separate the files into two lists: changing and unchanging */
    for (auto const& file : monitor_files_)
    {
        time_t const mtime = get_file_mtime(file);

        if (mtime + 2 >= now)
        {
            changing.push_back(file);
        }
        else
        {
            unchanging.push_back(file);
        }
    }

    /* add the files that have stopped changing */
    if (!unchanging.empty())
    {
        bool const do_start = gtr_pref_flag_get(TR_KEY_start_added_torrents);
        bool const do_prompt = gtr_pref_flag_get(TR_KEY_show_options_window);

        adding_from_watch_dir_ = true;
        add_files(unchanging, do_start, do_prompt, true);
        std::for_each(unchanging.begin(), unchanging.end(), rename_torrent);
        adding_from_watch_dir_ = false;
    }

    /* keep monitoring the ones that are still changing */
    monitor_files_ = changing;

    /* if monitor_files is nonempty, keep checking every second */
    if (!monitor_files_.empty())
    {
        return true;
    }

    monitor_idle_tag_.disconnect();
    return false;
}

/* If this file is a torrent, add it to our list */
void Session::Impl::watchdir_monitor_file(Glib::RefPtr<Gio::File> const& file)
{
    auto const filename = file->get_path();
    bool const is_torrent = Glib::str_has_suffix(filename, ".torrent");

    if (is_torrent)
    {
        /* if we're not already watching this file, start watching it now */
        bool const found = std::any_of(
            monitor_files_.begin(),
            monitor_files_.end(),
            [file](auto const& f) { return file->equal(f); });

        if (!found)
        {
            monitor_files_.push_back(file);

            if (!monitor_idle_tag_.connected())
            {
                monitor_idle_tag_ = Glib::signal_timeout().connect_seconds(sigc::mem_fun(*this, &Impl::watchdir_idle), 1);
            }
        }
    }
}

/* GFileMonitor noticed a file was created */
void Session::Impl::on_file_changed_in_watchdir(
    Glib::RefPtr<Gio::File> const& file,
    Glib::RefPtr<Gio::File> const& /*other_type*/,
    Gio::FileMonitorEvent event_type)
{
    if (event_type == Gio::FILE_MONITOR_EVENT_CREATED)
    {
        watchdir_monitor_file(file);
    }
}

/* walk through the pre-existing files in the watchdir */
void Session::Impl::watchdir_scan()
{
    auto const dirname = gtr_pref_string_get(TR_KEY_watch_dir);

    try
    {
        for (auto const& name : Glib::Dir(dirname))
        {
            watchdir_monitor_file(Gio::File::create_for_path(Glib::build_filename(dirname, name)));
        }
    }
    catch (Glib::FileError const&)
    {
    }
}

void Session::Impl::watchdir_update()
{
    bool const is_enabled = gtr_pref_flag_get(TR_KEY_watch_dir_enabled);
    auto const dir = Gio::File::create_for_path(gtr_pref_string_get(TR_KEY_watch_dir));

    if (monitor_ != nullptr && (!is_enabled || !dir->equal(monitor_dir_)))
    {
        monitor_tag_.disconnect();
        monitor_->cancel();

        monitor_dir_.reset();
        monitor_.reset();
    }

    if (is_enabled && monitor_ == nullptr)
    {
        auto const m = dir->monitor_directory();
        watchdir_scan();

        monitor_ = m;
        monitor_dir_ = dir;
        monitor_tag_ = m->signal_changed().connect(sigc::mem_fun(*this, &Impl::on_file_changed_in_watchdir));
    }
}

/***
****
***/

void Session::Impl::on_pref_changed(tr_quark const key)
{
    switch (key)
    {
    case TR_KEY_sort_mode:
    case TR_KEY_sort_reversed:
        {
            auto const mode = gtr_pref_string_get(TR_KEY_sort_mode);
            bool const is_reversed = gtr_pref_flag_get(TR_KEY_sort_reversed);
            set_sort_mode(mode, is_reversed);
            break;
        }

    case TR_KEY_peer_limit_global:
        tr_sessionSetPeerLimit(session_, gtr_pref_int_get(key));
        break;

    case TR_KEY_peer_limit_per_torrent:
        tr_sessionSetPeerLimitPerTorrent(session_, gtr_pref_int_get(key));
        break;

    case TR_KEY_inhibit_desktop_hibernation:
        maybe_inhibit_hibernation();
        break;

    case TR_KEY_watch_dir:
    case TR_KEY_watch_dir_enabled:
        watchdir_update();
        break;

    default:
        break;
    }
}

/**
***
**/

Glib::RefPtr<Session> Session::create(tr_session* session)
{
    return Glib::make_refptr_for_instance(new Session(session));
}

Session::Session(tr_session* session)
    : Glib::ObjectBase(typeid(Session))
    , impl_(std::make_unique<Impl>(*this, session))
{
}

Session::~Session() = default;

Session::Impl::Impl(Session& core, tr_session* session)
    : core_(core)
    , session_(session)
{
    raw_model_ = Gtk::ListStore::create(torrent_cols);
    sorted_model_ = Gtk::TreeModelSort::create(raw_model_);
    sorted_model_->set_default_sort_func([](Gtk::TreeModel::iterator const& /*a*/, Gtk::TreeModel::iterator const& /*b*/)
                                         { return 0; });

    /* init from prefs & listen to pref changes */
    on_pref_changed(TR_KEY_sort_mode);
    on_pref_changed(TR_KEY_sort_reversed);
    on_pref_changed(TR_KEY_watch_dir_enabled);
    on_pref_changed(TR_KEY_peer_limit_global);
    on_pref_changed(TR_KEY_inhibit_desktop_hibernation);
    signal_prefs_changed.connect([this](auto key) { on_pref_changed(key); });
}

tr_session* Session::close()
{
    return impl_->close();
}

tr_session* Session::Impl::close()
{
    auto* session = session_;

    if (session != nullptr)
    {
        session_ = nullptr;
        gtr_pref_save(session);
    }

    return session;
}

/***
****  COMPLETENESS CALLBACK
***/

/* this is called in the libtransmission thread, *NOT* the GTK+ thread,
   so delegate to the GTK+ thread before calling notify's dbus code... */
void Session::Impl::on_torrent_completeness_changed(tr_torrent* tor, tr_completeness completeness, bool was_running)
{
    if (was_running && completeness != TR_LEECH && tr_torrentStat(tor)->sizeWhenDone != 0)
    {
        Glib::signal_idle().connect(
            [core = get_core_ptr(), torrent_id = tr_torrentId(tor)]()
            {
                gtr_notify_torrent_completed(core, torrent_id);
                return false;
            });
    }
}

/***
****  METADATA CALLBACK
***/

namespace
{

Glib::ustring get_collated_name(tr_torrent const* tor)
{
    auto const* const inf = tr_torrentInfo(tor);
    return gtr_sprintf("%s\t%s", Glib::ustring(tr_torrentName(tor)).lowercase(), inf->hashString);
}

struct metadata_callback_data
{
    Session* core;
    int torrent_id;
};

Gtk::TreeModel::iterator find_row_from_torrent_id(Glib::RefPtr<Gtk::TreeModel> const& model, int id)
{
    for (auto const& row : model->children())
    {
        if (id == row.get_value(torrent_cols.torrent_id))
        {
            return row;
        }
    }

    return {};
}

} // namespace

/* this is called in the libtransmission thread, *NOT* the GTK+ thread,
   so delegate to the GTK+ thread before changing our list store... */
void Session::Impl::on_torrent_metadata_changed(tr_torrent* tor)
{
    Glib::signal_idle().connect(
        [this, core = get_core_ptr(), torrent_id = tr_torrentId(tor)]()
        {
            auto const* const tor2 = tr_torrentFindFromId(session_, torrent_id);

            /* update the torrent's collated name */
            if (tor2 != nullptr)
            {
                if (auto const iter = find_row_from_torrent_id(raw_model_, torrent_id); iter)
                {
                    (*iter)[torrent_cols.name_collated] = get_collated_name(tor2);
                }
            }

            return false;
        });
}

/***
****
****  ADDING TORRENTS
****
***/

namespace
{

unsigned int build_torrent_trackers_hash(tr_torrent* tor)
{
    uint64_t hash = 0;
    tr_info const* const inf = tr_torrentInfo(tor);

    for (unsigned int i = 0; i < inf->trackerCount; ++i)
    {
        for (char const* pch = inf->trackers[i].announce; *pch != '\0'; ++pch)
        {
            hash = (hash << 4) ^ (hash >> 28) ^ *pch;
        }
    }

    return hash;
}

bool is_torrent_active(tr_stat const* st)
{
    return st->peersSendingToUs > 0 || st->peersGettingFromUs > 0 || st->activity == TR_STATUS_CHECK;
}

} // namespace

void Session::add_torrent(tr_torrent* tor, bool do_notify)
{
    ScopedModelSortBlocker disable_sort(*gtr_get_ptr(impl_->get_model()));
    impl_->add_torrent(tor, do_notify);
}

void Session::Impl::add_torrent(tr_torrent* tor, bool do_notify)
{
    if (tor != nullptr)
    {
        tr_stat const* st = tr_torrentStat(tor);
        auto const collated = get_collated_name(tor);
        auto const trackers_hash = build_torrent_trackers_hash(tor);
        auto const store = get_raw_model();

        auto const iter = store->append();
        (*iter)[torrent_cols.name_collated] = collated;
        (*iter)[torrent_cols.torrent] = tor;
        (*iter)[torrent_cols.torrent_id] = tr_torrentId(tor);
        (*iter)[torrent_cols.speed_up] = st->pieceUploadSpeed_KBps;
        (*iter)[torrent_cols.speed_down] = st->pieceDownloadSpeed_KBps;
        (*iter)[torrent_cols.active_peers_up] = st->peersGettingFromUs;
        (*iter)[torrent_cols.active_peers_down] = st->peersSendingToUs + st->webseedsSendingToUs;
        (*iter)[torrent_cols.recheck_progress] = st->recheckProgress;
        (*iter)[torrent_cols.active] = is_torrent_active(st);
        (*iter)[torrent_cols.activity] = st->activity;
        (*iter)[torrent_cols.finished] = st->finished;
        (*iter)[torrent_cols.priority] = tr_torrentGetPriority(tor);
        (*iter)[torrent_cols.queue_position] = st->queuePosition;
        (*iter)[torrent_cols.trackers] = trackers_hash;

        if (do_notify)
        {
            gtr_notify_torrent_added(get_core_ptr(), tr_torrentId(tor));
        }

        tr_torrentSetMetadataCallback(
            tor,
            [](auto* tor2, void* impl) { static_cast<Impl*>(impl)->on_torrent_metadata_changed(tor2); },
            this);
        tr_torrentSetCompletenessCallback(
            tor,
            [](auto* tor2, auto completeness, bool was_running, void* impl)
            { static_cast<Impl*>(impl)->on_torrent_completeness_changed(tor2, completeness, was_running); },
            this);
    }
}

tr_torrent* Session::Impl::create_new_torrent(tr_ctor* ctor)
{
    bool do_trash = false;

    /* let the gtk client handle the removal, since libT
     * doesn't have any concept of the glib trash API */
    tr_ctorGetDeleteSource(ctor, &do_trash);
    tr_ctorSetDeleteSource(ctor, false);
    tr_torrent* const tor = tr_torrentNew(ctor, nullptr, nullptr);

    if (tor != nullptr && do_trash)
    {
        char const* config = tr_sessionGetConfigDir(session_);
        char const* source = tr_ctorGetSourceFile(ctor);

        if (source != nullptr)
        {
            /* #1294: don't delete the .torrent file if it's our internal copy */
            bool const is_internal = strstr(source, config) == source;

            if (!is_internal)
            {
                gtr_file_trash_or_remove(source, nullptr);
            }
        }
    }

    return tor;
}

int Session::Impl::add_ctor(tr_ctor* ctor, bool do_prompt, bool do_notify)
{
    tr_info inf;
    auto err = tr_torrentParse(ctor, &inf);

    switch (err)
    {
    case TR_PARSE_ERR:
        break;

    case TR_PARSE_DUPLICATE:
        /* don't complain about .torrent files in the watch directory
         * that have already been added... that gets annoying and we
         * don't want to be nagging users to clean up their watch dirs */
        if (tr_ctorGetSourceFile(ctor) == nullptr || !adding_from_watch_dir_)
        {
            signal_add_error.emit(static_cast<ErrorCode>(err), inf.name);
        }

        tr_metainfoFree(&inf);
        tr_ctorFree(ctor);
        break;

    default:
        if (do_prompt)
        {
            signal_add_prompt.emit(ctor);
        }
        else
        {
            ScopedModelSortBlocker disable_sort(*gtr_get_ptr(sorted_model_));
            add_torrent(create_new_torrent(ctor), do_notify);
            tr_ctorFree(ctor);
        }

        tr_metainfoFree(&inf);
        break;
    }

    return err;
}

namespace
{

void core_apply_defaults(tr_ctor* ctor)
{
    if (!tr_ctorGetPaused(ctor, TR_FORCE, nullptr))
    {
        tr_ctorSetPaused(ctor, TR_FORCE, !gtr_pref_flag_get(TR_KEY_start_added_torrents));
    }

    if (!tr_ctorGetDeleteSource(ctor, nullptr))
    {
        tr_ctorSetDeleteSource(ctor, gtr_pref_flag_get(TR_KEY_trash_original_torrent_files));
    }

    if (!tr_ctorGetPeerLimit(ctor, TR_FORCE, nullptr))
    {
        tr_ctorSetPeerLimit(ctor, TR_FORCE, gtr_pref_int_get(TR_KEY_peer_limit_per_torrent));
    }

    if (!tr_ctorGetDownloadDir(ctor, TR_FORCE, nullptr))
    {
        tr_ctorSetDownloadDir(ctor, TR_FORCE, gtr_pref_string_get(TR_KEY_download_dir).c_str());
    }
}

} // namespace

void Session::add_ctor(tr_ctor* ctor)
{
    bool const do_notify = false;
    bool const do_prompt = gtr_pref_flag_get(TR_KEY_show_options_window);
    core_apply_defaults(ctor);
    impl_->add_ctor(ctor, do_prompt, do_notify);
}

/***
****
***/

void Session::Impl::add_file_async_callback(
    Glib::RefPtr<Gio::File> const& file,
    Glib::RefPtr<Gio::AsyncResult>& result,
    tr_ctor* ctor,
    bool do_prompt,
    bool do_notify)
{
    gsize length;
    char* contents;

    try
    {
        if (!file->load_contents_finish(result, contents, length))
        {
            g_message(_("Couldn't read \"%s\""), file->get_parse_name().c_str());
        }
        else if (tr_ctorSetMetainfo(ctor, contents, length) == 0)
        {
            add_ctor(ctor, do_prompt, do_notify);
        }
        else
        {
            tr_ctorFree(ctor);
        }
    }
    catch (Glib::Error const& e)
    {
        g_message(_("Couldn't read \"%s\": %s"), file->get_parse_name().c_str(), e.what().c_str());
    }

    dec_busy();
}

bool Session::Impl::add_file(Glib::RefPtr<Gio::File> const& file, bool do_start, bool do_prompt, bool do_notify)
{
    bool handled = false;
    auto const* const session = get_session();

    if (session != nullptr)
    {
        tr_ctor* ctor;
        bool tried = false;
        bool loaded = false;

        ctor = tr_ctorNew(session);
        core_apply_defaults(ctor);
        tr_ctorSetPaused(ctor, TR_FORCE, !do_start);

        /* local files... */
        if (!tried)
        {
            auto const str = file->get_path();

            if ((tried = !str.empty() && Glib::file_test(str, Glib::FILE_TEST_EXISTS)))
            {
                loaded = !tr_ctorSetMetainfoFromFile(ctor, str.c_str());
            }
        }

        /* magnet links... */
        if (!tried && file->has_uri_scheme("magnet"))
        {
            /* GFile mangles the original string with /// so we have to un-mangle */
            auto const str = file->get_parse_name();
            auto const magnet = gtr_sprintf("magnet:%s", str.substr(str.find('?')));
            tried = true;
            loaded = !tr_ctorSetMetainfoFromMagnetLink(ctor, magnet.c_str());
        }

        /* hashcodes that we can turn into magnet links... */
        if (!tried)
        {
            auto const str = file->get_basename();

            if (gtr_is_hex_hashcode(str))
            {
                auto const magnet = gtr_sprintf("magnet:?xt=urn:btih:%s", str);
                loaded = !tr_ctorSetMetainfoFromMagnetLink(ctor, magnet.c_str());
            }
        }

        /* if we were able to load the metainfo, add the torrent */
        if (loaded)
        {
            handled = true;
            add_ctor(ctor, do_prompt, do_notify);
        }
        else if (file->has_uri_scheme("http") || file->has_uri_scheme("https") || file->has_uri_scheme("ftp"))
        {
            handled = true;
            inc_busy();
            file->load_contents_async([this, file, ctor, do_prompt, do_notify](auto& result)
                                      { add_file_async_callback(file, result, ctor, do_prompt, do_notify); });
        }
        else
        {
            tr_ctorFree(ctor);
            g_message(_("Skipping unknown torrent \"%s\""), file->get_parse_name().c_str());
        }
    }

    return handled;
}

bool Session::add_from_url(Glib::ustring const& uri)
{
    return impl_->add_from_url(uri);
}

bool Session::Impl::add_from_url(Glib::ustring const& uri)
{
    bool handled;
    bool const do_start = gtr_pref_flag_get(TR_KEY_start_added_torrents);
    bool const do_prompt = gtr_pref_flag_get(TR_KEY_show_options_window);
    bool const do_notify = false;

    auto const file = Gio::File::create_for_uri(uri);
    handled = add_file(file, do_start, do_prompt, do_notify);
    torrents_added();

    return handled;
}

void Session::add_files(std::vector<Glib::RefPtr<Gio::File>> const& files, bool do_start, bool do_prompt, bool do_notify)
{
    impl_->add_files(files, do_start, do_prompt, do_notify);
}

void Session::Impl::add_files(std::vector<Glib::RefPtr<Gio::File>> const& files, bool do_start, bool do_prompt, bool do_notify)
{
    for (auto const& file : files)
    {
        add_file(file, do_start, do_prompt, do_notify);
    }

    torrents_added();
}

void Session::torrents_added()
{
    impl_->torrents_added();
}

void Session::Impl::torrents_added()
{
    update();
    signal_add_error.emit(ERR_NO_MORE_TORRENTS, {});
}

void Session::torrent_changed(int id)
{
    auto const model = impl_->get_raw_model();

    if (auto const iter = find_row_from_torrent_id(model, id); iter)
    {
        model->row_changed(model->get_path(iter), iter);
    }
}

void Session::remove_torrent(int id, bool delete_local_data)
{
    auto* tor = find_torrent(id);

    if (tor != nullptr)
    {
        /* remove from the gui */
        auto const model = impl_->get_raw_model();

        if (auto const iter = find_row_from_torrent_id(model, id); iter)
        {
            model->erase(iter);
        }

        /* remove the torrent */
        tr_torrentRemove(
            tor,
            delete_local_data,
            [](char const* filename, tr_error** error) { return gtr_file_trash_or_remove(filename, error); });
    }
}

void Session::load(bool forcePaused)
{
    tr_ctor* ctor;
    tr_torrent** torrents;
    int count = 0;

    ctor = tr_ctorNew(impl_->get_session());

    if (forcePaused)
    {
        tr_ctorSetPaused(ctor, TR_FORCE, true);
    }

    tr_ctorSetPeerLimit(ctor, TR_FALLBACK, gtr_pref_int_get(TR_KEY_peer_limit_per_torrent));

    torrents = tr_sessionLoadTorrents(impl_->get_session(), ctor, &count);

    ScopedModelSortBlocker disable_sort(*gtr_get_ptr(impl_->get_model()));

    for (int i = 0; i < count; ++i)
    {
        impl_->add_torrent(torrents[i], false);
    }

    tr_free(torrents);
    tr_ctorFree(ctor);
}

void Session::clear()
{
    impl_->get_raw_model()->clear();
}

/***
****
***/

namespace
{

int gtr_compare_double(double const a, double const b, int decimal_places)
{
    int ret;
    int64_t const ia = (int64_t)(a * pow(10, decimal_places));
    int64_t const ib = (int64_t)(b * pow(10, decimal_places));

    if (ia < ib)
    {
        ret = -1;
    }
    else if (ia > ib)
    {
        ret = 1;
    }
    else
    {
        ret = 0;
    }

    return ret;
}

void update_foreach(Gtk::TreeModel::Row const& row)
{
    /* get the old states */
    auto* const tor = static_cast<tr_torrent*>(row.get_value(torrent_cols.torrent));
    auto const oldActive = row.get_value(torrent_cols.active);
    auto const oldActivePeerCount = row.get_value(torrent_cols.active_peer_count);
    auto const oldUploadPeerCount = row.get_value(torrent_cols.active_peers_up);
    auto const oldDownloadPeerCount = row.get_value(torrent_cols.active_peers_down);
    auto const oldError = row.get_value(torrent_cols.error);
    auto const oldActivity = row.get_value(torrent_cols.activity);
    auto const oldFinished = row.get_value(torrent_cols.finished);
    auto const oldPriority = row.get_value(torrent_cols.priority);
    auto const oldQueuePosition = row.get_value(torrent_cols.queue_position);
    auto const oldTrackers = row.get_value(torrent_cols.trackers);
    auto const oldUpSpeed = row.get_value(torrent_cols.speed_up);
    auto const oldRecheckProgress = row.get_value(torrent_cols.recheck_progress);
    auto const oldDownSpeed = row.get_value(torrent_cols.speed_down);

    /* get the new states */
    auto const* const st = tr_torrentStat(tor);
    auto const newActive = is_torrent_active(st);
    auto const newActivity = st->activity;
    auto const newFinished = st->finished;
    auto const newPriority = tr_torrentGetPriority(tor);
    auto const newQueuePosition = st->queuePosition;
    auto const newTrackers = build_torrent_trackers_hash(tor);
    auto const newUpSpeed = st->pieceUploadSpeed_KBps;
    auto const newDownSpeed = st->pieceDownloadSpeed_KBps;
    auto const newRecheckProgress = st->recheckProgress;
    auto const newActivePeerCount = st->peersSendingToUs + st->peersGettingFromUs + st->webseedsSendingToUs;
    auto const newDownloadPeerCount = st->peersSendingToUs;
    auto const newUploadPeerCount = st->peersGettingFromUs + st->webseedsSendingToUs;
    auto const newError = st->error;

    /* updating the model triggers off resort/refresh,
       so don't do it unless something's actually changed... */
    if (newActive != oldActive || newActivity != oldActivity || newFinished != oldFinished || newPriority != oldPriority ||
        newQueuePosition != oldQueuePosition || newError != oldError || newActivePeerCount != oldActivePeerCount ||
        newDownloadPeerCount != oldDownloadPeerCount || newUploadPeerCount != oldUploadPeerCount ||
        newTrackers != oldTrackers || gtr_compare_double(newUpSpeed, oldUpSpeed, 2) != 0 ||
        gtr_compare_double(newDownSpeed, oldDownSpeed, 2) != 0 ||
        gtr_compare_double(newRecheckProgress, oldRecheckProgress, 2) != 0)
    {
        row[torrent_cols.active] = newActive;
        row[torrent_cols.active_peer_count] = newActivePeerCount;
        row[torrent_cols.active_peers_up] = newUploadPeerCount;
        row[torrent_cols.active_peers_down] = newDownloadPeerCount;
        row[torrent_cols.error] = newError;
        row[torrent_cols.activity] = newActivity;
        row[torrent_cols.finished] = newFinished;
        row[torrent_cols.priority] = newPriority;
        row[torrent_cols.queue_position] = newQueuePosition;
        row[torrent_cols.trackers] = newTrackers;
        row[torrent_cols.speed_up] = newUpSpeed;
        row[torrent_cols.speed_down] = newDownSpeed;
        row[torrent_cols.recheck_progress] = newRecheckProgress;
    }
}

} // namespace

void Session::update()
{
    impl_->update();
}

void Session::start_now(int id)
{
    tr_variant top;
    tr_variantInitDict(&top, 2);
    tr_variantDictAddStrView(&top, TR_KEY_method, "torrent-start-now");

    auto args = tr_variantDictAddDict(&top, TR_KEY_arguments, 1);
    auto ids = tr_variantDictAddList(args, TR_KEY_ids, 1);
    tr_variantListAddInt(ids, id);
    exec(&top);
    tr_variantFree(&top);
}

void Session::Impl::update()
{
    /* update the model */
    for (auto const& row : raw_model_->children())
    {
        update_foreach(row);
    }

    /* update hibernation */
    maybe_inhibit_hibernation();
}

/**
***  Hibernate
**/

#define SESSION_MANAGER_SERVICE_NAME "org.gnome.SessionManager"
#define SESSION_MANAGER_INTERFACE "org.gnome.SessionManager"
#define SESSION_MANAGER_OBJECT_PATH "/org/gnome/SessionManager"

namespace
{

bool gtr_inhibit_hibernation(guint32& cookie)
{
    bool success = false;
    char const* application = "Transmission BitTorrent Client";
    char const* reason = "BitTorrent Activity";
    int const toplevel_xid = 0;
    int const flags = 4; /* Inhibit suspending the session or computer */

    try
    {
        auto const connection = Gio::DBus::Connection::get_sync(Gio::DBus::BUS_TYPE_SESSION);

        auto response = connection->call_sync(
            SESSION_MANAGER_OBJECT_PATH,
            SESSION_MANAGER_INTERFACE,
            "Inhibit",
            Glib::VariantContainerBase::create_tuple({
                Glib::Variant<Glib::ustring>::create(application),
                Glib::Variant<guint32>::create(toplevel_xid),
                Glib::Variant<Glib::ustring>::create(reason),
                Glib::Variant<guint32>::create(flags),
            }),
            SESSION_MANAGER_SERVICE_NAME,
            1000);

        cookie = Glib::VariantBase::cast_dynamic<Glib::Variant<guint32>>(response.get_child(0)).get();

        /* logging */
        tr_logAddInfo("%s", _("Inhibiting desktop hibernation"));

        success = true;
    }
    catch (Glib::Error const& e)
    {
        tr_logAddError(_("Couldn't inhibit desktop hibernation: %s"), e.what().c_str());
    }

    return success;
}

void gtr_uninhibit_hibernation(guint inhibit_cookie)
{
    try
    {
        auto const connection = Gio::DBus::Connection::get_sync(Gio::DBus::BUS_TYPE_SESSION);

        connection->call_sync(
            SESSION_MANAGER_OBJECT_PATH,
            SESSION_MANAGER_INTERFACE,
            "Uninhibit",
            Glib::VariantContainerBase::create_tuple({ Glib::Variant<guint32>::create(inhibit_cookie) }),
            SESSION_MANAGER_SERVICE_NAME,
            1000);

        /* logging */
        tr_logAddInfo("%s", _("Allowing desktop hibernation"));
    }
    catch (Glib::Error const& e)
    {
        g_warning("Couldn't uninhibit desktop hibernation: %s.", e.what().c_str());
    }
}

} // namespace

void Session::Impl::set_hibernation_allowed(bool allowed)
{
    inhibit_allowed_ = allowed;

    if (allowed && have_inhibit_cookie_)
    {
        gtr_uninhibit_hibernation(inhibit_cookie_);
        have_inhibit_cookie_ = false;
    }

    if (!allowed && !have_inhibit_cookie_ && !dbus_error_)
    {
        if (gtr_inhibit_hibernation(inhibit_cookie_))
        {
            have_inhibit_cookie_ = true;
        }
        else
        {
            dbus_error_ = true;
        }
    }
}

void Session::Impl::maybe_inhibit_hibernation()
{
    /* hibernation is allowed if EITHER
     * (a) the "inhibit" pref is turned off OR
     * (b) there aren't any active torrents */
    bool const hibernation_allowed = !gtr_pref_flag_get(TR_KEY_inhibit_desktop_hibernation) || get_active_torrent_count() == 0;
    set_hibernation_allowed(hibernation_allowed);
}

/**
***  Prefs
**/

void Session::Impl::commit_prefs_change(tr_quark const key)
{
    signal_prefs_changed.emit(key);
    gtr_pref_save(session_);
}

void Session::set_pref(tr_quark const key, std::string const& newval)
{
    if (newval != gtr_pref_string_get(key))
    {
        gtr_pref_string_set(key, newval);
        impl_->commit_prefs_change(key);
    }
}

void Session::set_pref(tr_quark const key, bool newval)
{
    if (newval != gtr_pref_flag_get(key))
    {
        gtr_pref_flag_set(key, newval);
        impl_->commit_prefs_change(key);
    }
}

void Session::set_pref(tr_quark const key, int newval)
{
    if (newval != gtr_pref_int_get(key))
    {
        gtr_pref_int_set(key, newval);
        impl_->commit_prefs_change(key);
    }
}

void Session::set_pref(tr_quark const key, double newval)
{
    if (gtr_compare_double(newval, gtr_pref_double_get(key), 4))
    {
        gtr_pref_double_set(key, newval);
        impl_->commit_prefs_change(key);
    }
}

/***
****
****  RPC Interface
****
***/

/* #define DEBUG_RPC */

namespace
{

int64_t nextTag = 1;

typedef void (*server_response_func)(Session* core, tr_variant* response, gpointer user_data);

std::map<int64_t, std::function<void(tr_variant*)>> pendingRequests;

bool core_read_rpc_response_idle(tr_variant* response)
{
    if (int64_t tag = 0; tr_variantDictFindInt(response, TR_KEY_tag, &tag))
    {
        if (auto const data_it = pendingRequests.find(tag); data_it != pendingRequests.end())
        {
            if (auto const& response_func = data_it->second; response_func)
            {
                response_func(response);
            }

            pendingRequests.erase(data_it);
        }
        else
        {
            g_warning("Pending RPC request for tag %" PRId64 " not found", tag);
        }
    }

    tr_variantFree(response);
    delete response;
    return false;
}

void core_read_rpc_response(tr_session* /*session*/, tr_variant* response, void* /*user_data*/)
{
    auto* response_copy = new tr_variant(std::move(*response));

    tr_variantInitBool(response, false);

    Glib::signal_idle().connect([response_copy]() { return core_read_rpc_response_idle(response_copy); });
}

} // namespace

void Session::Impl::send_rpc_request(
    tr_variant const* request,
    int64_t tag,
    std::function<void(tr_variant*)> const& response_func)
{
    if (session_ == nullptr)
    {
        g_error("GTK+ client doesn't support connections to remote servers yet.");
    }
    else
    {
        /* remember this request */
        pendingRequests.try_emplace(tag, response_func);

        /* make the request */
#ifdef DEBUG_RPC
        {
            struct evbuffer* buf = tr_variantToBuf(request, TR_VARIANT_FMT_JSON_LEAN);
            size_t const buf_len = evbuffer_get_length(buf);
            g_message("request: [%*.*s]", TR_ARG_TUPLE((int)buf_len, (int)buf_len, evbuffer_pullup(buf, -1)));
            evbuffer_free(buf);
        }
#endif

        tr_rpc_request_exec_json(session_, request, core_read_rpc_response, nullptr);
    }
}

/***
****  Sending a test-port request via RPC
***/

void Session::port_test()
{
    auto const tag = nextTag;
    ++nextTag;

    tr_variant request;
    tr_variantInitDict(&request, 2);
    tr_variantDictAddStrView(&request, TR_KEY_method, "port-test");
    tr_variantDictAddInt(&request, TR_KEY_tag, tag);
    impl_->send_rpc_request(
        &request,
        tag,
        [this](auto* response)
        {
            tr_variant* args;
            bool is_open;

            if (!tr_variantDictFindDict(response, TR_KEY_arguments, &args) ||
                !tr_variantDictFindBool(args, TR_KEY_port_is_open, &is_open))
            {
                is_open = false;
            }

            impl_->signal_port_tested.emit(is_open);
        });
    tr_variantFree(&request);
}

/***
****  Updating a blocklist via RPC
***/

void Session::blocklist_update()
{
    auto const tag = nextTag;
    ++nextTag;

    tr_variant request;
    tr_variantInitDict(&request, 2);
    tr_variantDictAddStrView(&request, TR_KEY_method, "blocklist-update");
    tr_variantDictAddInt(&request, TR_KEY_tag, tag);
    impl_->send_rpc_request(
        &request,
        tag,
        [this](auto* response)
        {
            tr_variant* args;
            int64_t ruleCount;

            if (!tr_variantDictFindDict(response, TR_KEY_arguments, &args) ||
                !tr_variantDictFindInt(args, TR_KEY_blocklist_size, &ruleCount))
            {
                ruleCount = -1;
            }

            if (ruleCount > 0)
            {
                gtr_pref_int_set(TR_KEY_blocklist_date, tr_time());
            }

            impl_->signal_blocklist_updated.emit(ruleCount);
        });
    tr_variantFree(&request);
}

/***
****
***/

void Session::exec(tr_variant const* top)
{
    auto const tag = nextTag;
    ++nextTag;

    impl_->send_rpc_request(top, tag, {});
}

/***
****
***/

size_t Session::get_torrent_count() const
{
    return impl_->get_raw_model()->children().size();
}

size_t Session::get_active_torrent_count() const
{
    return impl_->get_active_torrent_count();
}

size_t Session::Impl::get_active_torrent_count() const
{
    size_t activeCount = 0;

    for (auto const& row : raw_model_->children())
    {
        if (row.get_value(torrent_cols.activity) != TR_STATUS_STOPPED)
        {
            ++activeCount;
        }
    }

    return activeCount;
}

tr_torrent* Session::find_torrent(int id) const
{
    tr_torrent* tor = nullptr;

    if (auto* const session = impl_->get_session(); session != nullptr)
    {
        tor = tr_torrentFindFromId(session, id);
    }

    return tor;
}

void Session::open_folder(int torrent_id)
{
    auto const* tor = find_torrent(torrent_id);

    if (tor != nullptr)
    {
        bool const single = tr_torrentFileCount(tor) == 1;
        char const* currentDir = tr_torrentGetCurrentDir(tor);

        if (single)
        {
            gtr_open_file(currentDir);
        }
        else
        {
            gtr_open_file(Glib::build_filename(currentDir, tr_torrentName(tor)));
        }
    }
}

sigc::signal<void(Session::ErrorCode, Glib::ustring const&)>& Session::signal_add_error()
{
    return impl_->signal_add_error;
}

sigc::signal<void(tr_ctor*)>& Session::signal_add_prompt()
{
    return impl_->signal_add_prompt;
}

sigc::signal<void(int)>& Session::signal_blocklist_updated()
{
    return impl_->signal_blocklist_updated;
}

sigc::signal<void(bool)>& Session::signal_busy()
{
    return impl_->signal_busy;
}

sigc::signal<void(tr_quark)>& Session::signal_prefs_changed()
{
    return impl_->signal_prefs_changed;
}

sigc::signal<void(bool)>& Session::signal_port_tested()
{
    return impl_->signal_port_tested;
}
