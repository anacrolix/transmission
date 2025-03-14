/*
 * This file Copyright (C) 2013-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#define LIBTRANSMISSION_VARIANT_MODULE

#include <clocale> // setlocale()
#include <cstring> // strlen()
#include <string>
#include <string_view>

#include "transmission.h"
#include "utils.h" // tr_free()
#include "variant.h"
#include "variant-common.h"

#include "gtest/gtest.h"

using namespace std::literals;

class JSONTest : public ::testing::TestWithParam<char const*>
{
protected:
    void SetUp() override
    {
        auto const* locale_str = GetParam();
        if (setlocale(LC_NUMERIC, locale_str) == nullptr)
        {
            GTEST_SKIP();
        }
    }
};

TEST_P(JSONTest, testElements)
{
    auto const in = std::string{
        "{ \"string\": \"hello world\","
        "  \"escaped\": \"bell \\b formfeed \\f linefeed \\n carriage return \\r tab \\t\","
        "  \"int\": 5, "
        "  \"float\": 6.5, "
        "  \"true\": true, "
        "  \"false\": false, "
        "  \"null\": null }"
    };

    tr_variant top;
    EXPECT_TRUE(tr_variantFromBuf(&top, TR_VARIANT_PARSE_JSON | TR_VARIANT_PARSE_INPLACE, in));
    EXPECT_TRUE(tr_variantIsDict(&top));

    auto sv = std::string_view{};
    auto key = tr_quark_new("string"sv);
    EXPECT_TRUE(tr_variantDictFindStrView(&top, key, &sv));
    EXPECT_EQ("hello world"sv, sv);

    EXPECT_TRUE(tr_variantDictFindStrView(&top, tr_quark_new("escaped"sv), &sv));
    EXPECT_EQ("bell \b formfeed \f linefeed \n carriage return \r tab \t"sv, sv);

    auto i = int64_t{};
    EXPECT_TRUE(tr_variantDictFindInt(&top, tr_quark_new("int"sv), &i));
    EXPECT_EQ(5, i);

    auto d = double{};
    EXPECT_TRUE(tr_variantDictFindReal(&top, tr_quark_new("float"sv), &d));
    EXPECT_EQ(65, int(d * 10));

    auto f = bool{};
    EXPECT_TRUE(tr_variantDictFindBool(&top, tr_quark_new("true"sv), &f));
    EXPECT_TRUE(f);

    EXPECT_TRUE(tr_variantDictFindBool(&top, tr_quark_new("false"sv), &f));
    EXPECT_FALSE(f);

    EXPECT_TRUE(tr_variantDictFindStrView(&top, tr_quark_new("null"sv), &sv));
    EXPECT_EQ(""sv, sv);

    tr_variantFree(&top);
}

TEST_P(JSONTest, testUtf8)
{
    auto in = "{ \"key\": \"Letöltések\" }"sv;
    tr_variant top;
    auto sv = std::string_view{};
    char* json;
    tr_quark const key = tr_quark_new("key"sv);

    EXPECT_TRUE(tr_variantFromBuf(&top, TR_VARIANT_PARSE_JSON | TR_VARIANT_PARSE_INPLACE, in));
    EXPECT_TRUE(tr_variantIsDict(&top));
    EXPECT_TRUE(tr_variantDictFindStrView(&top, key, &sv));
    EXPECT_EQ("Letöltések"sv, sv);
    tr_variantFree(&top);

    in = R"({ "key": "\u005C" })"sv;
    EXPECT_TRUE(tr_variantFromBuf(&top, TR_VARIANT_PARSE_JSON | TR_VARIANT_PARSE_INPLACE, in));
    EXPECT_TRUE(tr_variantIsDict(&top));
    EXPECT_TRUE(tr_variantDictFindStrView(&top, key, &sv));
    EXPECT_EQ("\\"sv, sv);
    tr_variantFree(&top);

    /**
     * 1. Feed it JSON-escaped nonascii to the JSON decoder.
     * 2. Confirm that the result is UTF-8.
     * 3. Feed the same UTF-8 back into the JSON encoder.
     * 4. Confirm that the result is JSON-escaped.
     * 5. Dogfood that result back into the parser.
     * 6. Confirm that the result is UTF-8.
     */
    in = R"({ "key": "Let\u00f6lt\u00e9sek" })"sv;
    EXPECT_TRUE(tr_variantFromBuf(&top, TR_VARIANT_PARSE_JSON | TR_VARIANT_PARSE_INPLACE, in));
    EXPECT_TRUE(tr_variantIsDict(&top));
    EXPECT_TRUE(tr_variantDictFindStrView(&top, key, &sv));
    EXPECT_EQ("Letöltések"sv, sv);
    json = tr_variantToStr(&top, TR_VARIANT_FMT_JSON, nullptr);
    tr_variantFree(&top);

    EXPECT_NE(nullptr, json);
    EXPECT_NE(nullptr, strstr(json, "\\u00f6"));
    EXPECT_NE(nullptr, strstr(json, "\\u00e9"));
    EXPECT_TRUE(tr_variantFromBuf(&top, TR_VARIANT_PARSE_JSON | TR_VARIANT_PARSE_INPLACE, json));
    EXPECT_TRUE(tr_variantIsDict(&top));
    EXPECT_TRUE(tr_variantDictFindStrView(&top, key, &sv));
    EXPECT_EQ("Letöltések"sv, sv);
    tr_variantFree(&top);

    tr_free(json);
}

TEST_P(JSONTest, test1)
{
    auto const in = std::string{
        "{\n"
        "    \"headers\": {\n"
        "        \"type\": \"request\",\n"
        "        \"tag\": 666\n"
        "    },\n"
        "    \"body\": {\n"
        "        \"name\": \"torrent-info\",\n"
        "        \"arguments\": {\n"
        "            \"ids\": [ 7, 10 ]\n"
        "        }\n"
        "    }\n"
        "}\n"
    };

    tr_variant top;
    EXPECT_TRUE(tr_variantFromBuf(&top, TR_VARIANT_PARSE_JSON | TR_VARIANT_PARSE_INPLACE, in));

    auto sv = std::string_view{};
    int64_t i;
    EXPECT_TRUE(tr_variantIsDict(&top));
    auto* headers = tr_variantDictFind(&top, tr_quark_new("headers"sv));
    EXPECT_NE(nullptr, headers);
    EXPECT_TRUE(tr_variantIsDict(headers));
    EXPECT_TRUE(tr_variantDictFindStrView(headers, tr_quark_new("type"sv), &sv));
    EXPECT_EQ("request"sv, sv);
    EXPECT_TRUE(tr_variantDictFindInt(headers, TR_KEY_tag, &i));
    EXPECT_EQ(666, i);
    auto* body = tr_variantDictFind(&top, tr_quark_new("body"sv));
    EXPECT_NE(nullptr, body);
    EXPECT_TRUE(tr_variantDictFindStrView(body, TR_KEY_name, &sv));
    EXPECT_EQ("torrent-info"sv, sv);
    auto* args = tr_variantDictFind(body, tr_quark_new("arguments"sv));
    EXPECT_NE(nullptr, args);
    EXPECT_TRUE(tr_variantIsDict(args));
    auto* ids = tr_variantDictFind(args, TR_KEY_ids);
    EXPECT_NE(nullptr, ids);
    EXPECT_TRUE(tr_variantIsList(ids));
    EXPECT_EQ(2, tr_variantListSize(ids));
    EXPECT_TRUE(tr_variantGetInt(tr_variantListChild(ids, 0), &i));
    EXPECT_EQ(7, i);
    EXPECT_TRUE(tr_variantGetInt(tr_variantListChild(ids, 1), &i));
    EXPECT_EQ(10, i);

    tr_variantFree(&top);
}

TEST_P(JSONTest, test2)
{
    tr_variant top;
    auto const in = std::string{ " " };

    top.type = 0;
    EXPECT_FALSE(tr_variantFromBuf(&top, TR_VARIANT_PARSE_JSON | TR_VARIANT_PARSE_INPLACE, in));
    EXPECT_FALSE(tr_variantIsDict(&top));
}

TEST_P(JSONTest, test3)
{
    auto const
        in = "{ \"error\": 2,"
             "  \"errorString\": \"torrent not registered with this tracker 6UHsVW'*C\","
             "  \"eta\": 262792,"
             "  \"id\": 25,"
             "  \"leftUntilDone\": 2275655680 }"sv;

    tr_variant top;
    EXPECT_TRUE(tr_variantFromBuf(&top, TR_VARIANT_PARSE_JSON | TR_VARIANT_PARSE_INPLACE, in));

    auto sv = std::string_view{};
    EXPECT_TRUE(tr_variantDictFindStrView(&top, TR_KEY_errorString, &sv));
    EXPECT_EQ("torrent not registered with this tracker 6UHsVW'*C"sv, sv);

    tr_variantFree(&top);
}

TEST_P(JSONTest, unescape)
{
    tr_variant top;
    auto const in = std::string{ R"({ "string-1": "\/usr\/lib" })" };
    EXPECT_TRUE(tr_variantFromBuf(&top, TR_VARIANT_PARSE_JSON | TR_VARIANT_PARSE_INPLACE, in));

    auto sv = std::string_view{};
    EXPECT_TRUE(tr_variantDictFindStrView(&top, tr_quark_new("string-1"sv), &sv));
    EXPECT_EQ("/usr/lib"sv, sv);

    tr_variantFree(&top);
}

INSTANTIATE_TEST_SUITE_P( //
    JSON,
    JSONTest,
    ::testing::Values( //
        "C",
        "da_DK.UTF-8",
        "fr_FR.UTF-8",
        "ru_RU.UTF-8"));
