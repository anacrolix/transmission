/*
 * This file Copyright (C) 2013-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <array>
#include <string_view>

#include "transmission.h"
#include "clients.h"
#include "crypto-utils.h"

#include "gtest/gtest.h"

using namespace std::literals;

TEST(Client, clientForId)
{
    struct LocalTest
    {
        std::string_view peer_id;
        std::string_view expected_client;
    };

    auto constexpr Tests = std::array<LocalTest, 37>{
        { { "-AZ8421-"sv, "Azureus / Vuze 8.4.2.1"sv },
          { "-BC0241-"sv, "BitComet 2.41"sv }, // two major, two minor
          { "-BI2300-"sv, "BiglyBT 2.3.0.0"sv },
          { "-BL246326"sv, "BitLord 2.4.6-326"sv }, // Style used after BitLord 0.59
          { "-BN0001-"sv, "Baidu Netdisk"sv }, // Baidu Netdisk Client v5.5.4
          { "-BT791B-"sv, "BitTorrent 7.9.1 (Beta)"sv },
          { "-BT791\0-"sv, "BitTorrent 7.9.1"sv },
          { "-FC1013-"sv, "FileCroc 1.0.1.3"sv },
          { "-FC1013-"sv, "FileCroc 1.0.1.3"sv },
          { "-FD51@\xFF-"sv, "Free Download Manager 5.1.x"sv }, // Negative test case
          { "-FD51R\xFF-"sv, "Free Download Manager 5.1.27"sv },
          { "-FD51W\xFF-"sv, "Free Download Manager 5.1.32"sv },
          { "-FL51FF-"sv, "Folx 5.x"sv }, // Folx v5.2.1.13690
          { "-FW6830-"sv, "FrostWire 6.8.3"sv },
          { "-IIO\x10\x2D\x04-"sv, "-IIO%10-%04-"sv },
          { "-I\05O\x08\x03\x01-"sv, "-I%05O%08%03%01-"sv },
          { "-KT33D1-"sv, "KTorrent 3.3 Dev 1"sv },
          { "-MR1100-"sv, "Miro 1.1.0.0"sv },
          { "-PI0091-"sv, "PicoTorrent 0.09.1"sv },
          { "-PI0120-"sv, "PicoTorrent 0.12.0"sv },
          { "-TR0006-"sv, "Transmission 0.6"sv },
          { "-TR0072-"sv, "Transmission 0.72"sv },
          { "-TR111Z-"sv, "Transmission 1.11+"sv },
          { "-UT341\0-"sv, "\xc2\xb5Torrent 3.4.1"sv },
          { "-UW110Q-"sv, "\xc2\xb5Torrent Web 1.1.0"sv },
          { "-UW1110Q"sv, "\xc2\xb5Torrent Web 1.1.10"sv }, // wider version
          { "-WS1000-"sv, "HTTP Seed"sv },
          { "-WW0007-"sv, "WebTorrent 0.0.0.7"sv },
          { "-XF9990-"sv, "Xfplay 9.9.9"sv }, // Older Xfplay versions have three digit version number
          { "-XF9992-"sv, "Xfplay 9.9.92"sv }, // Xfplay 9.9.92 to 9.9.94 uses "-XF9992-"
          { "A2-1-18-8-"sv, "aria2 1.18.8"sv },
          { "A2-1-2-0-"sv, "aria2 1.2.0"sv },
          { "S58B-----"sv, "Shad0w 5.8.11"sv },
          { "Q1-23-4-"sv, "Queen Bee 1.23.4"sv },
          { "TIX0193-"sv, "Tixati 1.93"sv },
          { "\x65\x78\x62\x63\x00\x38\x4C\x4F\x52\x44\x32\x00\x04\x8E\xCE\xD5\x7B\xD7\x10\x28"sv, "BitLord 0.56"sv },
          { "\x65\x78\x62\x63\x00\x38\x7A\x44\x63\x10\x2D\x6E\x9A\xD6\x72\x3B\x33\x9F\x35\xA9"sv, "BitComet 0.56"sv } }
    };

    for (auto const& test : Tests)
    {
        auto peer_id = tr_peer_id_t{};
        tr_rand_buffer(std::data(peer_id), std::size(peer_id));
        std::copy(std::begin(test.peer_id), std::end(test.peer_id), std::begin(peer_id));

        auto buf = std::array<char, 128>{};
        tr_clientForId(buf.data(), buf.size(), peer_id);
        EXPECT_EQ(test.expected_client, buf.data());
    }
}
