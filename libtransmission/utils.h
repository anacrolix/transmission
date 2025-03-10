/*
 * This file Copyright (C) 2009-2017 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdarg>
#include <cstddef>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "platform-quota.h"
#include "tr-macros.h"

/***
****
***/

struct evbuffer;
struct event;
struct timeval;
struct tr_error;

/**
 * @addtogroup utils Utilities
 * @{
 */

char const* tr_strip_positional_args(char const* fmt);

#if !defined(_)
#if defined(HAVE_LIBINTL_H) && !defined(__APPLE__)
#include <libintl.h>
#define _(a) gettext(a)
#else
#define _(a) (a)
#endif
#endif

/* #define DISABLE_GETTEXT */
#ifndef DISABLE_GETTEXT
#if defined(_WIN32) || defined(TR_LIGHTWEIGHT)
#define DISABLE_GETTEXT
#endif
#endif
#ifdef DISABLE_GETTEXT
#undef _
#define _(a) tr_strip_positional_args(a)
#endif

/****
*****
****/

#define TR_N_ELEMENTS(ary) (sizeof(ary) / sizeof(*(ary)))

std::string_view tr_get_mime_type_for_filename(std::string_view filename);

/**
 * @brief Rich Salz's classic implementation of shell-style pattern matching for ?, \, [], and * characters.
 * @return 1 if the pattern matches, 0 if it doesn't, or -1 if an error occured
 */
bool tr_wildmat(char const* text, char const* pattern) TR_GNUC_NONNULL(1, 2);

/**
 * @brief Loads a file and returns its contents.
 * On failure, NULL is returned and errno is set.
 */
uint8_t* tr_loadFile(char const* filename, size_t* size, struct tr_error** error) TR_GNUC_MALLOC TR_GNUC_NONNULL(1);

bool tr_loadFile(std::vector<char>& setme, char const* filename, tr_error** error = nullptr);

bool tr_saveFile(char const* filename_in, std::string_view contents, tr_error** error = nullptr);

/** @brief build a filename from a series of elements using the
           platform's correct directory separator. */
char* tr_buildPath(char const* first_element, ...) TR_GNUC_NULL_TERMINATED TR_GNUC_MALLOC;

template<typename... T, typename std::enable_if_t<(std::is_convertible_v<T, std::string_view> && ...), bool> = true>
std::string& tr_buildBuf(std::string& setme, T... args)
{
    setme.clear();
    auto const n = (std::size(std::string_view{ args }) + ...);
    if (setme.capacity() < n)
    {
        setme.reserve(n);
    }
    ((setme += args), ...);
    return setme;
}

/**
 * @brief Get disk capacity and free disk space (in bytes) for the specified folder.
 * @return struct with free and total as zero or positive integer on success, -1 in case of error.
 */
tr_disk_space tr_dirSpace(std::string_view path);

/**
 * @brief Convenience wrapper around timer_add() to have a timer wake up in a number of seconds and microseconds
 * @param timer         the timer to set
 * @param seconds       seconds to wait
 * @param microseconds  microseconds to wait
 */
void tr_timerAdd(struct event* timer, int seconds, int microseconds) TR_GNUC_NONNULL(1);

/**
 * @brief Convenience wrapper around timer_add() to have a timer wake up in a number of milliseconds
 * @param timer         the timer to set
 * @param milliseconds  milliseconds to wait
 */
void tr_timerAddMsec(struct event* timer, int milliseconds) TR_GNUC_NONNULL(1);

/** @brief return the current date in milliseconds */
uint64_t tr_time_msec(void);

/** @brief sleep the specified number of milliseconds */
void tr_wait_msec(long int delay_milliseconds);

/**
 * @brief make a copy of 'str' whose non-utf8 content has been corrected or stripped
 * @return a newly-allocated string that must be freed with tr_free()
 * @param str the string to make a clean copy of
 */
char* tr_utf8clean(std::string_view str) TR_GNUC_MALLOC;

bool tr_utf8_validate(std::string_view sv, char const** endptr);

#ifdef _WIN32

char* tr_win32_native_to_utf8(wchar_t const* text, int text_size);
char* tr_win32_native_to_utf8_ex(
    wchar_t const* text,
    int text_size,
    int extra_chars_before,
    int extra_chars_after,
    int* real_result_size);
wchar_t* tr_win32_utf8_to_native(char const* text, int text_size);
wchar_t* tr_win32_utf8_to_native_ex(
    char const* text,
    int text_size,
    int extra_chars_before,
    int extra_chars_after,
    int* real_result_size);
char* tr_win32_format_message(uint32_t code);

void tr_win32_make_args_utf8(int* argc, char*** argv);

int tr_main_win32(int argc, char** argv, int (*real_main)(int, char**));

#define tr_main(...) \
    main_impl(__VA_ARGS__); \
    int main(int argc, char* argv[]) \
    { \
        return tr_main_win32(argc, argv, &main_impl); \
    } \
    int main_impl(__VA_ARGS__)

#else

#define tr_main main

#endif

/***
****
***/

/** @brief Portability wrapper around malloc() in which `0' is a safe argument */
void* tr_malloc(size_t size);

/** @brief Portability wrapper around calloc() in which `0' is a safe argument */
void* tr_malloc0(size_t size);

/** @brief Portability wrapper around reallocf() in which `0' is a safe argument */
void* tr_realloc(void* p, size_t size);

/** @brief Portability wrapper around free() in which `nullptr' is a safe argument */
void tr_free(void* p);

/** @brief Free pointers in a nullptr-terminated array (the array itself is not freed) */
void tr_free_ptrv(void* const* p);

/**
 * @brief make a newly-allocated copy of a chunk of memory
 * @param src the memory to copy
 * @param byteCount the number of bytes to copy
 * @return a newly-allocated copy of `src' that can be freed with tr_free()
 */
void* tr_memdup(void const* src, size_t byteCount);

#define tr_new(struct_type, n_structs) (static_cast<struct_type*>(tr_malloc(sizeof(struct_type) * (size_t)(n_structs))))

#define tr_new0(struct_type, n_structs) (static_cast<struct_type*>(tr_malloc0(sizeof(struct_type) * (size_t)(n_structs))))

#define tr_renew(struct_type, mem, n_structs) \
    (static_cast<struct_type*>(tr_realloc((mem), sizeof(struct_type) * (size_t)(n_structs))))

/**
 * @brief make a newly-allocated copy of a substring
 * @param in is a void* so that callers can pass in both signed & unsigned without a cast
 * @param len length of the substring to copy. if a length less than zero is passed in, strlen(len) is used
 * @return a newly-allocated copy of `in' that can be freed with tr_free()
 */
char* tr_strndup(void const* in, size_t len) TR_GNUC_MALLOC;

/**
 * @brief make a newly-allocated copy of a string
 * @param in is a void* so that callers can pass in both signed & unsigned without a cast
 * @return a newly-allocated copy of `in' that can be freed with tr_free()
 */
char* tr_strdup(void const* in);

/**
 * @brief like strcmp() but gracefully handles nullptr strings
 */
int tr_strcmp0(char const* str1, char const* str2);

constexpr bool tr_str_is_empty(char const* value)
{
    return value == nullptr || *value == '\0';
}

char* evbuffer_free_to_str(struct evbuffer* buf, size_t* result_len);

/**
 * @brief sprintf() a string into a newly-allocated buffer large enough to hold it
 * @return a newly-allocated string that can be freed with tr_free()
 */
char* tr_strdup_printf(char const* fmt, ...) TR_GNUC_MALLOC TR_GNUC_PRINTF(1, 2);
char* tr_strdup_vprintf(char const* fmt, va_list args) TR_GNUC_MALLOC TR_GNUC_PRINTF(1, 0);

/** @brief Portability wrapper for strlcpy() that uses the system implementation if available */
size_t tr_strlcpy(void* dst, void const* src, size_t siz);

/** @brief Portability wrapper for snprintf() that uses the system implementation if available */
int tr_snprintf(void* buf, size_t buflen, char const* fmt, ...) TR_GNUC_PRINTF(3, 4) TR_GNUC_NONNULL(1, 3);

/** @brief Convenience wrapper around strerorr() guaranteed to not return nullptr
    @param errnum the error number to describe */
char const* tr_strerror(int errnum);

/** @brief Returns true if the string ends with the specified case-insensitive suffix */
bool tr_str_has_suffix(char const* str, char const* suffix);

/** @brief Portability wrapper for memmem() that uses the system implementation if available */
char const* tr_memmem(char const* haystack, size_t haystack_len, char const* needle, size_t needle_len);

/** @brief Portability wrapper for strcasestr() that uses the system implementation if available */
char const* tr_strcasestr(char const* haystack, char const* needle);

template<typename T>
std::string tr_strlower(T in)
{
    auto out = std::string{ in };
    std::for_each(std::begin(out), std::end(out), [](char& ch) { ch = std::tolower(ch); });
    return out;
}

/***
****  std::string_view utils
***/

template<typename... T, typename std::enable_if_t<(std::is_convertible_v<T, std::string_view> && ...), bool> = true>
std::string tr_strvPath(T... args)
{
    auto setme = std::string{};
    auto const n_args = sizeof...(args);
    auto const n = n_args + (std::size(std::string_view{ args }) + ...);
    if (setme.capacity() < n)
    {
        setme.reserve(n);
    }

    auto const foo = [&setme](std::string_view a)
    {
        setme += a;
        setme += TR_PATH_DELIMITER;
    };
    (foo(args), ...);
    setme.resize(setme.size() - 1);
    return setme;
}

template<typename... T, typename std::enable_if_t<(std::is_convertible_v<T, std::string_view> && ...), bool> = true>
std::string tr_strvJoin(T... args)
{
    auto setme = std::string{};
    auto const n = (std::size(std::string_view{ args }) + ...);
    if (setme.capacity() < n)
    {
        setme.reserve(n);
    }
    ((setme += args), ...);
    return setme;
}

template<typename T>
constexpr bool tr_strvContains(std::string_view sv, T key) // c++23
{
    return sv.find(key) != sv.npos;
}

constexpr bool tr_strvStartsWith(std::string_view sv, char key) // c++20
{
    return !std::empty(sv) && sv.front() == key;
}

constexpr bool tr_strvStartsWith(std::string_view sv, std::string_view key) // c++20
{
    return std::size(key) <= std::size(sv) && sv.substr(0, std::size(key)) == key;
}

constexpr bool tr_strvEndsWith(std::string_view sv, std::string_view key) // c++20
{
    return std::size(key) <= std::size(sv) && sv.substr(std::size(sv) - std::size(key)) == key;
}

constexpr bool tr_strvEndsWith(std::string_view sv, char key) // c++20
{
    return !std::empty(sv) && sv.back() == key;
}

constexpr std::string_view tr_strvSep(std::string_view* sv, char delim)
{
    auto pos = sv->find(delim);
    auto const ret = sv->substr(0, pos);
    sv->remove_prefix(pos != sv->npos ? pos + 1 : std::size(*sv));
    return ret;
}

constexpr bool tr_strvSep(std::string_view* sv, std::string_view* token, char delim)
{
    return !std::empty((*token = tr_strvSep(sv, delim)));
}

std::string_view tr_strvStrip(std::string_view sv);

char* tr_strvDup(std::string_view) TR_GNUC_MALLOC;

std::string tr_strvUtf8Clean(std::string_view sv);

/***
****
***/

/** @brief return TR_RATIO_NA, TR_RATIO_INF, or a number in [0..1]
    @return TR_RATIO_NA, TR_RATIO_INF, or a number in [0..1] */
double tr_getRatio(uint64_t numerator, uint64_t denominator);

/**
 * @brief Given a string like "1-4" or "1-4,6,9,14-51", this returns a
 *        newly-allocated array of all the integers in the set.
 * @return a newly-allocated array of integers that must be freed with tr_free(),
 *         or nullptr if a fragment of the string can't be parsed.
 *
 * For example, "5-8" will return [ 5, 6, 7, 8 ] and setmeCount will be 4.
 */
std::vector<int> tr_parseNumberRange(std::string_view str);

/**
 * @brief truncate a double value at a given number of decimal places.
 *
 * this can be used to prevent a printf() call from rounding up:
 * call with the decimal_places argument equal to the number of
 * decimal places in the printf()'s precision:
 *
 * - printf("%.2f%%", 99.999) ==> "100.00%"
 *
 * - printf("%.2f%%", tr_truncd(99.999, 2)) ==> "99.99%"
 *             ^                        ^
 *             |   These should match   |
 *             +------------------------+
 */
double tr_truncd(double x, int decimal_places);

/* return a percent formatted string of either x.xx, xx.x or xxx */
char* tr_strpercent(char* buf, double x, size_t buflen);

/**
 * @param buf      the buffer to write the string to
 * @param buflen   buf's size
 * @param ratio    the ratio to convert to a string
 * @param infinity the string represntation of "infinity"
 */
char* tr_strratio(char* buf, size_t buflen, double ratio, char const* infinity) TR_GNUC_NONNULL(1, 4);

/** @brief Portability wrapper for localtime_r() that uses the system implementation if available */
struct tm* tr_localtime_r(time_t const* _clock, struct tm* _result);

/** @brief Portability wrapper for gmtime_r() that uses the system implementation if available */
struct tm* tr_gmtime_r(time_t const* _clock, struct tm* _result);

/** @brief Portability wrapper for gettimeofday(), with tz argument dropped */
int tr_gettimeofday(struct timeval* tv);

/**
 * @brief move a file
 * @return `True` on success, `false` otherwise (with `error` set accordingly).
 */
bool tr_moveFile(char const* oldpath, char const* newpath, struct tr_error** error) TR_GNUC_NONNULL(1, 2);

/** @brief convenience function to remove an item from an array */
void tr_removeElementFromArray(void* array, size_t index_to_remove, size_t sizeof_element, size_t nmemb);

/***
****
***/

/** @brief Private libtransmission variable that's visible only for inlining in tr_time() */
extern time_t __tr_current_time;

/**
 * @brief very inexpensive form of time(nullptr)
 * @return the current epoch time in seconds
 *
 * This function returns a second counter that is updated once per second.
 * If something blocks the libtransmission thread for more than a second,
 * that counter may be thrown off, so this function is not guaranteed
 * to always be accurate. However, it is *much* faster when 100% accuracy
 * isn't needed
 */
static inline time_t tr_time(void)
{
    return __tr_current_time;
}

/** @brief Private libtransmission function to update tr_time()'s counter */
constexpr void tr_timeUpdate(time_t now)
{
    __tr_current_time = now;
}

/** @brief Portability wrapper for htonll() that uses the system implementation if available */
uint64_t tr_htonll(uint64_t);

/** @brief Portability wrapper for htonll() that uses the system implementation if available */
uint64_t tr_ntohll(uint64_t);

/***
****
***/

/* example: tr_formatter_size_init(1024, _("KiB"), _("MiB"), _("GiB"), _("TiB")); */

void tr_formatter_size_init(uint64_t kilo, char const* kb, char const* mb, char const* gb, char const* tb);

void tr_formatter_speed_init(size_t kilo, char const* kb, char const* mb, char const* gb, char const* tb);

void tr_formatter_mem_init(size_t kilo, char const* kb, char const* mb, char const* gb, char const* tb);

extern size_t tr_speed_K;
extern size_t tr_mem_K;
extern uint64_t tr_size_K; /* unused? */

/* format a speed from KBps into a user-readable string. */
char* tr_formatter_speed_KBps(char* buf, double KBps, size_t buflen);

/* format a memory size from bytes into a user-readable string. */
char* tr_formatter_mem_B(char* buf, size_t bytes, size_t buflen);

/* format a memory size from MB into a user-readable string. */
static inline char* tr_formatter_mem_MB(char* buf, double MBps, size_t buflen)
{
    return tr_formatter_mem_B(buf, (size_t)(MBps * tr_mem_K * tr_mem_K), buflen);
}

/* format a file size from bytes into a user-readable string. */
char* tr_formatter_size_B(char* buf, uint64_t bytes, size_t buflen);

void tr_formatter_get_units(void* dict);

/***
****
***/

/** @brief Check if environment variable exists. */
bool tr_env_key_exists(char const* key);

/** @brief Get environment variable value as int. */
int tr_env_get_int(char const* key, int default_value);

/** @brief Get environment variable value as string (should be freed afterwards). */
char* tr_env_get_string(char const* key, char const* default_value);

/***
****
***/

void tr_net_init(void);
