/*
 * This file Copyright (C) 2009-2015 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#pragma once

#include <array>

#include <QObject>
#include <QSet>
#include <QString>
#include <QVariant>

#include <libtransmission/quark.h>

#include "Filters.h"
#include "Macros.h"

class QDateTime;

extern "C"
{
    struct tr_variant;
}

class Prefs : public QObject
{
    Q_OBJECT
    TR_DISABLE_COPY_MOVE(Prefs)

public:
    enum
    {
        /* client prefs */
        OPTIONS_PROMPT,
        OPEN_DIALOG_FOLDER,
        INHIBIT_HIBERNATION,
        DIR_WATCH,
        DIR_WATCH_ENABLED,
        SHOW_TRAY_ICON,
        START_MINIMIZED,
        SHOW_NOTIFICATION_ON_ADD,
        SHOW_NOTIFICATION_ON_COMPLETE,
        ASKQUIT,
        SORT_MODE,
        SORT_REVERSED,
        COMPACT_VIEW,
        FILTERBAR,
        STATUSBAR,
        STATUSBAR_STATS,
        SHOW_TRACKER_SCRAPES,
        SHOW_BACKUP_TRACKERS,
        TOOLBAR,
        BLOCKLIST_DATE,
        BLOCKLIST_UPDATES_ENABLED,
        MAIN_WINDOW_LAYOUT_ORDER,
        MAIN_WINDOW_HEIGHT,
        MAIN_WINDOW_WIDTH,
        MAIN_WINDOW_X,
        MAIN_WINDOW_Y,
        FILTER_MODE,
        FILTER_TRACKERS,
        FILTER_TEXT,
        SESSION_IS_REMOTE,
        SESSION_REMOTE_HOST,
        SESSION_REMOTE_PORT,
        SESSION_REMOTE_AUTH,
        SESSION_REMOTE_USERNAME,
        SESSION_REMOTE_PASSWORD,
        COMPLETE_SOUND_COMMAND,
        COMPLETE_SOUND_ENABLED,
        USER_HAS_GIVEN_INFORMED_CONSENT,
        /* core prefs */
        FIRST_CORE_PREF,
        ALT_SPEED_LIMIT_UP = FIRST_CORE_PREF,
        ALT_SPEED_LIMIT_DOWN,
        ALT_SPEED_LIMIT_ENABLED,
        ALT_SPEED_LIMIT_TIME_BEGIN,
        ALT_SPEED_LIMIT_TIME_END,
        ALT_SPEED_LIMIT_TIME_ENABLED,
        ALT_SPEED_LIMIT_TIME_DAY,
        BLOCKLIST_ENABLED,
        BLOCKLIST_URL,
        DSPEED,
        DSPEED_ENABLED,
        DOWNLOAD_DIR,
        DOWNLOAD_QUEUE_ENABLED,
        DOWNLOAD_QUEUE_SIZE,
        ENCRYPTION,
        IDLE_LIMIT,
        IDLE_LIMIT_ENABLED,
        INCOMPLETE_DIR,
        INCOMPLETE_DIR_ENABLED,
        MSGLEVEL,
        PEER_LIMIT_GLOBAL,
        PEER_LIMIT_TORRENT,
        PEER_PORT,
        PEER_PORT_RANDOM_ON_START,
        PEER_PORT_RANDOM_LOW,
        PEER_PORT_RANDOM_HIGH,
        QUEUE_STALLED_MINUTES,
        SCRIPT_TORRENT_DONE_ENABLED,
        SCRIPT_TORRENT_DONE_FILENAME,
        SOCKET_TOS,
        START,
        TRASH_ORIGINAL,
        PEX_ENABLED,
        DHT_ENABLED,
        UTP_ENABLED,
        LPD_ENABLED,
        PORT_FORWARDING,
        PREALLOCATION,
        RATIO,
        RATIO_ENABLED,
        RENAME_PARTIAL_FILES,
        RPC_AUTH_REQUIRED,
        RPC_ENABLED,
        RPC_PASSWORD,
        RPC_PORT,
        RPC_USERNAME,
        RPC_WHITELIST_ENABLED,
        RPC_WHITELIST,
        USPEED_ENABLED,
        USPEED,
        UPLOAD_SLOTS_PER_TORRENT,
        LAST_CORE_PREF = UPLOAD_SLOTS_PER_TORRENT,
        //
        PREFS_COUNT
    };

    explicit Prefs(QString config_dir);
    ~Prefs() override;

    bool isCore(int key) const
    {
        return FIRST_CORE_PREF <= key && key <= LAST_CORE_PREF;
    }

    bool isClient(int key) const
    {
        return !isCore(key);
    }

    tr_quark getKey(int i) const
    {
        return Items[i].key;
    }

    int type(int i) const
    {
        return Items[i].type;
    }

    QVariant const& variant(int i) const
    {
        return values_[i];
    }

    int getInt(int key) const;
    bool getBool(int key) const;
    QString getString(int key) const;
    double getDouble(int key) const;
    QDateTime getDateTime(int key) const;

    template<typename T>
    T get(int key) const
    {
        return values_[key].value<T>();
    }

    template<typename T>
    void set(int key, T const& value)
    {
        QVariant& v(values_[key]);
        QVariant const tmp = QVariant::fromValue(value);

        if (v.isNull() || v != tmp)
        {
            v = tmp;
            emit changed(key);
        }
    }

    void toggleBool(int key);

signals:
    void changed(int key);

private:
    struct PrefItem
    {
        int id;
        tr_quark key;
        int type;
    };

    void initDefaults(tr_variant*) const;

    void set(int key, char const* value) = delete;

    QString const config_dir_;

    QSet<int> temporary_prefs_;
    std::array<QVariant, PREFS_COUNT> mutable values_;

    static std::array<PrefItem, PREFS_COUNT> const Items;
};
