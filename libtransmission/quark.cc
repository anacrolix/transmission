/*
 * This file Copyright (C) 2013-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <algorithm>
#include <array>
#include <cstring> // strlen()
#include <iterator>
#include <string_view>
#include <vector>

#include "transmission.h"
#include "quark.h"
#include "tr-assert.h"
#include "utils.h" // tr_strndup()

using namespace std::literals;

namespace
{

auto constexpr my_static = std::array<std::string_view, 392>{ ""sv,
                                                              "activeTorrentCount"sv,
                                                              "activity-date"sv,
                                                              "activityDate"sv,
                                                              "added"sv,
                                                              "added-date"sv,
                                                              "added.f"sv,
                                                              "added6"sv,
                                                              "added6.f"sv,
                                                              "addedDate"sv,
                                                              "address"sv,
                                                              "alt-speed-down"sv,
                                                              "alt-speed-enabled"sv,
                                                              "alt-speed-time-begin"sv,
                                                              "alt-speed-time-day"sv,
                                                              "alt-speed-time-enabled"sv,
                                                              "alt-speed-time-end"sv,
                                                              "alt-speed-up"sv,
                                                              "announce"sv,
                                                              "announce-list"sv,
                                                              "announceState"sv,
                                                              "anti-brute-force-enabled"sv,
                                                              "anti-brute-force-threshold"sv,
                                                              "arguments"sv,
                                                              "bandwidth-priority"sv,
                                                              "bandwidthPriority"sv,
                                                              "bind-address-ipv4"sv,
                                                              "bind-address-ipv6"sv,
                                                              "bitfield"sv,
                                                              "blocklist-date"sv,
                                                              "blocklist-enabled"sv,
                                                              "blocklist-size"sv,
                                                              "blocklist-updates-enabled"sv,
                                                              "blocklist-url"sv,
                                                              "blocks"sv,
                                                              "bytesCompleted"sv,
                                                              "cache-size-mb"sv,
                                                              "clientIsChoked"sv,
                                                              "clientIsInterested"sv,
                                                              "clientName"sv,
                                                              "comment"sv,
                                                              "comment_utf_8"sv,
                                                              "compact-view"sv,
                                                              "complete"sv,
                                                              "config-dir"sv,
                                                              "cookies"sv,
                                                              "corrupt"sv,
                                                              "corruptEver"sv,
                                                              "created by"sv,
                                                              "created by.utf-8"sv,
                                                              "creation date"sv,
                                                              "creator"sv,
                                                              "cumulative-stats"sv,
                                                              "current-stats"sv,
                                                              "date"sv,
                                                              "dateCreated"sv,
                                                              "delete-local-data"sv,
                                                              "desiredAvailable"sv,
                                                              "destination"sv,
                                                              "details-window-height"sv,
                                                              "details-window-width"sv,
                                                              "dht-enabled"sv,
                                                              "display-name"sv,
                                                              "dnd"sv,
                                                              "done-date"sv,
                                                              "doneDate"sv,
                                                              "download-dir"sv,
                                                              "download-dir-free-space"sv,
                                                              "download-queue-enabled"sv,
                                                              "download-queue-size"sv,
                                                              "downloadCount"sv,
                                                              "downloadDir"sv,
                                                              "downloadLimit"sv,
                                                              "downloadLimited"sv,
                                                              "downloadSpeed"sv,
                                                              "downloaded"sv,
                                                              "downloaded-bytes"sv,
                                                              "downloadedBytes"sv,
                                                              "downloadedEver"sv,
                                                              "downloaders"sv,
                                                              "downloading-time-seconds"sv,
                                                              "dropped"sv,
                                                              "dropped6"sv,
                                                              "e"sv,
                                                              "editDate"sv,
                                                              "encoding"sv,
                                                              "encryption"sv,
                                                              "error"sv,
                                                              "errorString"sv,
                                                              "eta"sv,
                                                              "etaIdle"sv,
                                                              "failure reason"sv,
                                                              "fields"sv,
                                                              "file-count"sv,
                                                              "fileStats"sv,
                                                              "filename"sv,
                                                              "files"sv,
                                                              "files-added"sv,
                                                              "files-unwanted"sv,
                                                              "files-wanted"sv,
                                                              "filesAdded"sv,
                                                              "filter-mode"sv,
                                                              "filter-text"sv,
                                                              "filter-trackers"sv,
                                                              "flagStr"sv,
                                                              "flags"sv,
                                                              "format"sv,
                                                              "fromCache"sv,
                                                              "fromDht"sv,
                                                              "fromIncoming"sv,
                                                              "fromLpd"sv,
                                                              "fromLtep"sv,
                                                              "fromPex"sv,
                                                              "fromTracker"sv,
                                                              "hasAnnounced"sv,
                                                              "hasScraped"sv,
                                                              "hashString"sv,
                                                              "have"sv,
                                                              "haveUnchecked"sv,
                                                              "haveValid"sv,
                                                              "honorsSessionLimits"sv,
                                                              "host"sv,
                                                              "id"sv,
                                                              "idle-limit"sv,
                                                              "idle-mode"sv,
                                                              "idle-seeding-limit"sv,
                                                              "idle-seeding-limit-enabled"sv,
                                                              "ids"sv,
                                                              "incomplete"sv,
                                                              "incomplete-dir"sv,
                                                              "incomplete-dir-enabled"sv,
                                                              "info"sv,
                                                              "info_hash"sv,
                                                              "inhibit-desktop-hibernation"sv,
                                                              "interval"sv,
                                                              "ip"sv,
                                                              "ipv4"sv,
                                                              "ipv6"sv,
                                                              "isBackup"sv,
                                                              "isDownloadingFrom"sv,
                                                              "isEncrypted"sv,
                                                              "isFinished"sv,
                                                              "isIncoming"sv,
                                                              "isPrivate"sv,
                                                              "isStalled"sv,
                                                              "isUTP"sv,
                                                              "isUploadingTo"sv,
                                                              "labels"sv,
                                                              "lastAnnouncePeerCount"sv,
                                                              "lastAnnounceResult"sv,
                                                              "lastAnnounceStartTime"sv,
                                                              "lastAnnounceSucceeded"sv,
                                                              "lastAnnounceTime"sv,
                                                              "lastAnnounceTimedOut"sv,
                                                              "lastScrapeResult"sv,
                                                              "lastScrapeStartTime"sv,
                                                              "lastScrapeSucceeded"sv,
                                                              "lastScrapeTime"sv,
                                                              "lastScrapeTimedOut"sv,
                                                              "leecherCount"sv,
                                                              "leftUntilDone"sv,
                                                              "length"sv,
                                                              "location"sv,
                                                              "lpd-enabled"sv,
                                                              "m"sv,
                                                              "magnet-info"sv,
                                                              "magnetLink"sv,
                                                              "main-window-height"sv,
                                                              "main-window-is-maximized"sv,
                                                              "main-window-layout-order"sv,
                                                              "main-window-width"sv,
                                                              "main-window-x"sv,
                                                              "main-window-y"sv,
                                                              "manualAnnounceTime"sv,
                                                              "max-peers"sv,
                                                              "maxConnectedPeers"sv,
                                                              "memory-bytes"sv,
                                                              "memory-units"sv,
                                                              "message-level"sv,
                                                              "metadataPercentComplete"sv,
                                                              "metadata_size"sv,
                                                              "metainfo"sv,
                                                              "method"sv,
                                                              "min interval"sv,
                                                              "min_request_interval"sv,
                                                              "move"sv,
                                                              "msg_type"sv,
                                                              "mtimes"sv,
                                                              "name"sv,
                                                              "name.utf-8"sv,
                                                              "nextAnnounceTime"sv,
                                                              "nextScrapeTime"sv,
                                                              "nodes"sv,
                                                              "nodes6"sv,
                                                              "open-dialog-dir"sv,
                                                              "p"sv,
                                                              "path"sv,
                                                              "path.utf-8"sv,
                                                              "paused"sv,
                                                              "pausedTorrentCount"sv,
                                                              "peer-congestion-algorithm"sv,
                                                              "peer-id-ttl-hours"sv,
                                                              "peer-limit"sv,
                                                              "peer-limit-global"sv,
                                                              "peer-limit-per-torrent"sv,
                                                              "peer-port"sv,
                                                              "peer-port-random-high"sv,
                                                              "peer-port-random-low"sv,
                                                              "peer-port-random-on-start"sv,
                                                              "peer-socket-tos"sv,
                                                              "peerIsChoked"sv,
                                                              "peerIsInterested"sv,
                                                              "peers"sv,
                                                              "peers2"sv,
                                                              "peers2-6"sv,
                                                              "peers6"sv,
                                                              "peersConnected"sv,
                                                              "peersFrom"sv,
                                                              "peersGettingFromUs"sv,
                                                              "peersSendingToUs"sv,
                                                              "percentDone"sv,
                                                              "pex-enabled"sv,
                                                              "piece"sv,
                                                              "piece length"sv,
                                                              "pieceCount"sv,
                                                              "pieceSize"sv,
                                                              "pieces"sv,
                                                              "play-download-complete-sound"sv,
                                                              "port"sv,
                                                              "port-forwarding-enabled"sv,
                                                              "port-is-open"sv,
                                                              "preallocation"sv,
                                                              "prefetch-enabled"sv,
                                                              "primary-mime-type"sv,
                                                              "priorities"sv,
                                                              "priority"sv,
                                                              "priority-high"sv,
                                                              "priority-low"sv,
                                                              "priority-normal"sv,
                                                              "private"sv,
                                                              "progress"sv,
                                                              "prompt-before-exit"sv,
                                                              "queue-move-bottom"sv,
                                                              "queue-move-down"sv,
                                                              "queue-move-top"sv,
                                                              "queue-move-up"sv,
                                                              "queue-stalled-enabled"sv,
                                                              "queue-stalled-minutes"sv,
                                                              "queuePosition"sv,
                                                              "rateDownload"sv,
                                                              "rateToClient"sv,
                                                              "rateToPeer"sv,
                                                              "rateUpload"sv,
                                                              "ratio-limit"sv,
                                                              "ratio-limit-enabled"sv,
                                                              "ratio-mode"sv,
                                                              "recent-download-dir-1"sv,
                                                              "recent-download-dir-2"sv,
                                                              "recent-download-dir-3"sv,
                                                              "recent-download-dir-4"sv,
                                                              "recheckProgress"sv,
                                                              "remote-session-enabled"sv,
                                                              "remote-session-host"sv,
                                                              "remote-session-password"sv,
                                                              "remote-session-port"sv,
                                                              "remote-session-requres-authentication"sv,
                                                              "remote-session-username"sv,
                                                              "removed"sv,
                                                              "rename-partial-files"sv,
                                                              "reqq"sv,
                                                              "result"sv,
                                                              "rpc-authentication-required"sv,
                                                              "rpc-bind-address"sv,
                                                              "rpc-enabled"sv,
                                                              "rpc-host-whitelist"sv,
                                                              "rpc-host-whitelist-enabled"sv,
                                                              "rpc-password"sv,
                                                              "rpc-port"sv,
                                                              "rpc-url"sv,
                                                              "rpc-username"sv,
                                                              "rpc-version"sv,
                                                              "rpc-version-minimum"sv,
                                                              "rpc-version-semver"sv,
                                                              "rpc-whitelist"sv,
                                                              "rpc-whitelist-enabled"sv,
                                                              "scrape"sv,
                                                              "scrape-paused-torrents-enabled"sv,
                                                              "scrapeState"sv,
                                                              "script-torrent-added-enabled"sv,
                                                              "script-torrent-added-filename"sv,
                                                              "script-torrent-done-enabled"sv,
                                                              "script-torrent-done-filename"sv,
                                                              "seconds-active"sv,
                                                              "secondsActive"sv,
                                                              "secondsDownloading"sv,
                                                              "secondsSeeding"sv,
                                                              "seed-queue-enabled"sv,
                                                              "seed-queue-size"sv,
                                                              "seedIdleLimit"sv,
                                                              "seedIdleMode"sv,
                                                              "seedRatioLimit"sv,
                                                              "seedRatioLimited"sv,
                                                              "seedRatioMode"sv,
                                                              "seederCount"sv,
                                                              "seeding-time-seconds"sv,
                                                              "session-count"sv,
                                                              "session-id"sv,
                                                              "sessionCount"sv,
                                                              "show-backup-trackers"sv,
                                                              "show-extra-peer-details"sv,
                                                              "show-filterbar"sv,
                                                              "show-notification-area-icon"sv,
                                                              "show-options-window"sv,
                                                              "show-statusbar"sv,
                                                              "show-toolbar"sv,
                                                              "show-tracker-scrapes"sv,
                                                              "size-bytes"sv,
                                                              "size-units"sv,
                                                              "sizeWhenDone"sv,
                                                              "sort-mode"sv,
                                                              "sort-reversed"sv,
                                                              "source"sv,
                                                              "speed"sv,
                                                              "speed-Bps"sv,
                                                              "speed-bytes"sv,
                                                              "speed-limit-down"sv,
                                                              "speed-limit-down-enabled"sv,
                                                              "speed-limit-up"sv,
                                                              "speed-limit-up-enabled"sv,
                                                              "speed-units"sv,
                                                              "start-added-torrents"sv,
                                                              "start-minimized"sv,
                                                              "startDate"sv,
                                                              "status"sv,
                                                              "statusbar-stats"sv,
                                                              "tag"sv,
                                                              "tier"sv,
                                                              "time-checked"sv,
                                                              "torrent-added"sv,
                                                              "torrent-added-notification-command"sv,
                                                              "torrent-added-notification-enabled"sv,
                                                              "torrent-complete-notification-command"sv,
                                                              "torrent-complete-notification-enabled"sv,
                                                              "torrent-complete-sound-command"sv,
                                                              "torrent-complete-sound-enabled"sv,
                                                              "torrent-duplicate"sv,
                                                              "torrent-get"sv,
                                                              "torrent-set"sv,
                                                              "torrent-set-location"sv,
                                                              "torrentCount"sv,
                                                              "torrentFile"sv,
                                                              "torrents"sv,
                                                              "totalSize"sv,
                                                              "total_size"sv,
                                                              "tracker id"sv,
                                                              "trackerAdd"sv,
                                                              "trackerRemove"sv,
                                                              "trackerReplace"sv,
                                                              "trackerStats"sv,
                                                              "trackers"sv,
                                                              "trash-can-enabled"sv,
                                                              "trash-original-torrent-files"sv,
                                                              "umask"sv,
                                                              "units"sv,
                                                              "upload-slots-per-torrent"sv,
                                                              "uploadLimit"sv,
                                                              "uploadLimited"sv,
                                                              "uploadRatio"sv,
                                                              "uploadSpeed"sv,
                                                              "upload_only"sv,
                                                              "uploaded"sv,
                                                              "uploaded-bytes"sv,
                                                              "uploadedBytes"sv,
                                                              "uploadedEver"sv,
                                                              "url-list"sv,
                                                              "use-global-speed-limit"sv,
                                                              "use-speed-limit"sv,
                                                              "user-has-given-informed-consent"sv,
                                                              "ut_comment"sv,
                                                              "ut_holepunch"sv,
                                                              "ut_metadata"sv,
                                                              "ut_pex"sv,
                                                              "ut_recommend"sv,
                                                              "utp-enabled"sv,
                                                              "v"sv,
                                                              "version"sv,
                                                              "wanted"sv,
                                                              "warning message"sv,
                                                              "watch-dir"sv,
                                                              "watch-dir-enabled"sv,
                                                              "webseeds"sv,
                                                              "webseedsSendingToUs"sv };

size_t constexpr quarks_are_sorted = ( //
    []() constexpr
    {
        for (size_t i = 1; i < std::size(my_static); ++i)
        {
            if (my_static[i - 1] >= my_static[i])
            {
                return false;
            }
        }

        return true;
    })();

static_assert(quarks_are_sorted, "Predefined quarks must be sorted by their string value");
static_assert(std::size(my_static) == TR_N_KEYS);

auto& my_runtime{ *new std::vector<std::string_view>{} };

} // namespace

std::optional<tr_quark> tr_quark_lookup(std::string_view key)
{
    // is it in our static array?
    auto constexpr sbegin = std::begin(my_static), send = std::end(my_static);
    auto const sit = std::lower_bound(sbegin, send, key);
    if (sit != send && *sit == key)
    {
        return std::distance(sbegin, sit);
    }

    /* was it added during runtime? */
    auto const rbegin = std::begin(my_runtime), rend = std::end(my_runtime);
    auto const rit = std::find(rbegin, rend, key);
    if (rit != rend)
    {
        return TR_N_KEYS + std::distance(rbegin, rit);
    }

    return {};
}

tr_quark tr_quark_new(std::string_view str)
{
    if (auto const prior = tr_quark_lookup(str); prior)
    {
        return *prior;
    }

    auto const ret = TR_N_KEYS + std::size(my_runtime);
    my_runtime.emplace_back(tr_strndup(std::data(str), std::size(str)), std::size(str));
    return ret;
}

std::string_view tr_quark_get_string_view(tr_quark q)
{
    return q < TR_N_KEYS ? my_static[q] : my_runtime[q - TR_N_KEYS];
}

char const* tr_quark_get_string(tr_quark q, size_t* len)
{
    auto const tmp = tr_quark_get_string_view(q);

    if (len != nullptr)
    {
        *len = std::size(tmp);
    }

    return std::data(tmp);
}
