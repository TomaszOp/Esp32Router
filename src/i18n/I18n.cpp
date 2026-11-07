/**
 * @file I18n.cpp
 * @brief Language versions: current language, ASCII conversion, padding.
 */
#include "i18n/I18n.h"

#include <cstring>

namespace i18n {

void setLanguage(Lang lang)
{
    current.store(lang, std::memory_order_relaxed);
}

bool parse(const std::string& code, Lang* out)
{
    if (code == "pl") {
        *out = Lang::PL;
        return true;
    }
    if (code == "en") {
        *out = Lang::EN;
        return true;
    }
    return false;
}

const char* code(Lang lang)
{
    return lang == Lang::EN ? "en" : "pl";
}

namespace {

struct Map {
    const char* utf8;
    const char* ascii;
};

// Polish letters and characters used in messages.
constexpr Map MAP[] = {
    { "ą", "a" }, { "ć", "c" }, { "ę", "e" }, { "ł", "l" }, { "ń", "n" }, { "ó", "o" }, { "ś", "s" },
    { "ź", "z" }, { "ż", "z" }, { "Ą", "A" }, { "Ć", "C" }, { "Ę", "E" }, { "Ł", "L" }, { "Ń", "N" },
    { "Ó", "O" }, { "Ś", "S" }, { "Ź", "Z" }, { "Ż", "Z" }, { "–", "-" }, { "—", "-" }, { "→", "->" },
    { "←", "<-" }, { "„", "\"" }, { "”", "\"" }, { "“", "\"" }, { "…", "..." }, { "·", "-" },
    { "°", " st." }, { "×", "x" }, { "✓", "+" }, { "✗", "x" },
};

} // namespace

const char* asciiFor(const unsigned char* p, size_t n, size_t* used)
{
    for (const Map& m : MAP) {
        size_t len = strlen(m.utf8);
        if (len <= n && memcmp(p, m.utf8, len) == 0) {
            *used = len;
            return m.ascii;
        }
    }
    // Unknown character: skip the whole UTF-8 sequence.
    size_t len = 1;
    while (len < n && len < 4 && (p[len] & 0xC0) == 0x80) {
        ++len;
    }
    *used = len;
    return "?";
}

std::string pad(const char* s, size_t width)
{
    std::string out(s);
    size_t chars = 0;
    for (unsigned char c : out) {
        chars += (c & 0xC0) != 0x80;
    }
    if (chars < width) {
        out.append(width - chars, ' ');
    }
    return out;
}

std::string ascii(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    toAscii(s.data(), s.size(), [&](const char* d, size_t n) { out.append(d, n); });
    return out;
}

} // namespace i18n
