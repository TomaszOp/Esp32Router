/**
 * @file I18n.h
 * @brief Language versions of messages (doc/dev/jezyki.md).
 *
 * One language for the panel and the console – global.json "language". Texts are pairs at the
 * place of use: tr("po polsku", "in English") – both versions in flash, no table and no lookup.
 * In printf formats both versions must have the same specifiers (checked by tools/i18n_check.py).
 * Logs (LOG_x) are not translated.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

/// Language versions: current language, ASCII conversion for the console, translation helpers.
namespace i18n {

/// Supported languages.
enum class Lang : uint8_t { PL, EN };

inline std::atomic<Lang> current{ Lang::PL }; ///< Current language (set by GlobalSettings, read from any task).
inline std::atomic<bool> consoleUtf8{ false }; ///< global.console_utf8: COM/Telnet console without ASCII conversion.

/// @return The current language.
inline Lang language() { return current.load(std::memory_order_relaxed); }
/// @brief Sets the current language (GlobalSettings: at start-up and on a live change of "language").
void setLanguage(Lang lang);
/// @brief Parses a language code. @return false for anything other than "pl" / "en".
bool parse(const std::string& code, Lang* out);
/// @return Language code "pl" / "en".
const char* code(Lang lang);

/**
 * @brief Polish letters and typographic UTF-8 characters converted to ASCII (COM/Telnet console, logs).
 *
 * Other non-ASCII characters -> '?'. Calls out(fragment, length) for consecutive pieces, without allocation.
 */
template <typename Out>
void toAscii(const char* data, size_t len, Out&& out);
/// @return Copy of the string converted to ASCII (used for translated fragments in logs).
std::string ascii(const std::string& s);

/// @brief COM/Telnet console output: UTF-8 unchanged (console_utf8) or converted to ASCII.
template <typename Out>
void consoleWrite(const char* data, size_t len, Out&& out)
{
    if (consoleUtf8.load(std::memory_order_relaxed)) {
        out(data, len);
    } else {
        toAscii(data, len, out);
    }
}

/**
 * @brief Text padded with spaces to width characters (not bytes – Polish letters take 2 bytes in UTF-8).
 *
 * Use instead of "%-11s" for table columns with translated text: printf("%s", pad(text, 11).c_str()).
 */
std::string pad(const char* s, size_t width);

/// @brief A text pair passed on and stored in both versions (e.g. a configuration field hint, a command description).
struct Text {
    const char* pl = nullptr; ///< Polish text.
    const char* en = nullptr; ///< English text (nullptr = use Polish).

    /// @return The text in the current language.
    const char* get() const { return language() == Lang::EN && en != nullptr ? en : pl; }
    /// @return true when a text is set.
    explicit operator bool() const { return pl != nullptr; }

};

/**
 * @brief Converts one UTF-8 character starting at p (n bytes to the end).
 * @return ASCII replacement; *used receives the sequence length. Internal – for toAscii().
 */
const char* asciiFor(const unsigned char* p, size_t n, size_t* used);

template <typename Out>
void toAscii(const char* data, size_t len, Out&& out)
{
    const char* start = data;
    const char* end = data + len;
    const char* p = data;
    while (p < end) {
        if (static_cast<unsigned char>(*p) < 0x80) {
            ++p;
            continue;
        }
        if (p > start) {
            out(start, p - start);
        }
        size_t used = 1;
        const char* rep = asciiFor(reinterpret_cast<const unsigned char*>(p), end - p, &used);
        size_t replen = 0;
        while (rep[replen] != '\0') {
            ++replen;
        }
        out(rep, replen);
        p += used;
        start = p;
    }
    if (p > start) {
        out(start, p - start);
    }
}

} // namespace i18n

/**
 * @brief Text in the current language.
 *
 * format_arg: the compiler checks the Polish version of a printf format.
 */

__attribute__((format_arg(1))) inline const char* tr(const char* pl, const char* en)
{
    return i18n::language() == i18n::Lang::EN ? en : pl;
}
