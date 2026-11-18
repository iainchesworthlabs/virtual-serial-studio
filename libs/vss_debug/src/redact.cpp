//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/debug/redact.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string_view>

namespace vss::debug
{
namespace
{

constexpr std::size_t shortest_literal = 3;
constexpr std::string_view marker = "<redacted>";

char lower(char letter) noexcept
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
}

/// The path separators are alike for this comparison: a Windows path may
/// have been written with either.
bool same_letter(char a, char b) noexcept
{
    if ((a == '\\' || a == '/') && (b == '\\' || b == '/'))
        return true;
    return lower(a) == lower(b);
}

std::string replace_literal(std::string text, const std::string& needle,
                            const std::string& replacement)
{
    std::string out;
    out.reserve(text.size());
    std::size_t at = 0;
    while (at < text.size())
    {
        if (at + needle.size() <= text.size() &&
            std::equal(needle.begin(), needle.end(),
                       text.begin() + static_cast<std::ptrdiff_t>(at),
                       same_letter))
        {
            out += replacement;
            at += needle.size();
        }
        else
        {
            out += text[at];
            ++at;
        }
    }
    return out;
}

// The credential shapes are found by hand rather than with std::regex: the
// scanners are linear in the length of the text, and the standard library's
// regex machinery draws a false warning from some compilers.

bool is_space(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
           c == '\r';
}

bool is_alpha(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_alnum(char c) noexcept
{
    return is_alpha(c) || (c >= '0' && c <= '9');
}

bool is_word(char c) noexcept
{
    return is_alnum(c) || c == '_';
}

bool is_scheme_char(char c) noexcept
{
    return is_alnum(c) || c == '+' || c == '.' || c == '-';
}

bool is_bearer_char(char c) noexcept
{
    return is_alnum(c) || c == '.' || c == '_' || c == '~' || c == '+' ||
           c == '/' || c == '-';
}

bool is_secret_char(char c) noexcept
{
    return !is_space(c) && c != ',' && c != ';' && c != '"' && c != '\'' &&
           c != '&';
}

bool is_user_char(char c) noexcept
{
    return c != '/' && !is_space(c) && c != ':' && c != '@';
}

bool is_password_char(char c) noexcept
{
    return c != '/' && !is_space(c) && c != '@';
}

bool is_equals(char c) noexcept
{
    return c == '=';
}

/// True when `word` (lower case) is at `at`, whatever the case of the text.
bool word_at(const std::string& text, std::size_t at, std::string_view word)
{
    if (at > text.size() || word.size() > text.size() - at)
        return false;
    for (std::size_t index = 0; index < word.size(); ++index)
    {
        if (lower(text[at + index]) != word[index])
            return false;
    }
    return true;
}

/// The end of the run from `at` of characters that pass `keep`.
template <class Keep>
std::size_t skip_while(const std::string& text, std::size_t at, Keep keep)
{
    while (at < text.size() && keep(text[at]))
        ++at;
    return at;
}

/// What a scanner found: the text up to `keep_end` stays, and the rest, up to
/// `end`, is replaced.
struct hit
{
    std::size_t keep_end = 0;
    std::size_t end = 0;
};

/// After a name: optional space, `=` or `:`, optional space.  Returns where
/// the value starts, or `npos`.
std::size_t after_assignment(const std::string& text, std::size_t at)
{
    at = skip_while(text, at, is_space);
    if (at >= text.size() || (text[at] != '=' && text[at] != ':'))
        return std::string::npos;
    return skip_while(text, at + 1, is_space);
}

/// `password=`, `token:`, `api_key =` and the like, then the value.
std::optional<hit> named_secret_at(const std::string& text, std::size_t at)
{
    std::size_t after = std::string::npos;
    if (word_at(text, at, "password"))
        after = at + 8;
    else if (word_at(text, at, "token"))
        after = at + 5;
    else if (word_at(text, at, "passwd") || word_at(text, at, "secret") ||
             word_at(text, at, "apikey"))
        after = at + 6;
    else if (word_at(text, at, "api"))
    {
        after = at + 3;
        if (after < text.size() && (text[after] == '_' || text[after] == '-'))
            ++after;
        if (!word_at(text, after, "key"))
            return std::nullopt;
        after += 3;
    }
    if (after == std::string::npos)
        return std::nullopt;
    const std::size_t value = after_assignment(text, after);
    if (value == std::string::npos)
        return std::nullopt;
    const std::size_t end = skip_while(text, value, is_secret_char);
    if (end == value)
        return std::nullopt;
    return hit{value, end};
}

/// `bearer` and a token, with any `=` that ends it.
std::optional<hit> bearer_at(const std::string& text, std::size_t at)
{
    if (!word_at(text, at, "bearer"))
        return std::nullopt;
    const std::size_t gap = at + 6;
    const std::size_t value = skip_while(text, gap, is_space);
    if (value == gap)
        return std::nullopt;
    std::size_t end = skip_while(text, value, is_bearer_char);
    if (end == value)
        return std::nullopt;
    end = skip_while(text, end, is_equals);
    return hit{value, end};
}

/// `serial=`, `serialnumber:` or `ser=` on a word boundary, then the value.
std::optional<hit> serial_number_at(const std::string& text, std::size_t at)
{
    if (at > 0 && is_word(text[at - 1]))
        return std::nullopt;
    std::size_t value = std::string::npos;
    if (word_at(text, at, "serialnumber"))
        value = after_assignment(text, at + 12);
    if (value == std::string::npos && word_at(text, at, "serial"))
        value = after_assignment(text, at + 6);
    if (value == std::string::npos && word_at(text, at, "ser"))
        value = after_assignment(text, at + 3);
    if (value == std::string::npos)
        return std::nullopt;
    const std::size_t end = skip_while(text, value, is_alnum);
    if (end == value)
        return std::nullopt;
    return hit{value, end};
}

/// Replaces what `find` reports at each position, left to right.
template <class Find>
std::string replace_hits(const std::string& text, Find find)
{
    std::string out;
    out.reserve(text.size());
    std::size_t at = 0;
    while (at < text.size())
    {
        if (const auto found = find(text, at))
        {
            out.append(text, at, found->keep_end - at);
            out += marker;
            at = found->end;
        }
        else
        {
            out += text[at];
            ++at;
        }
    }
    return out;
}

/// `scheme://user:password@`: the user and password are replaced, and the
/// scheme stays.  Looks for each `://` and reads back to the start of its
/// scheme, so the work stays linear however long the text is.
std::string redact_url_passwords(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    std::size_t copied = 0;
    std::size_t search = 0;
    while (true)
    {
        const std::size_t colon = text.find("://", search);
        if (colon == std::string::npos)
            break;
        search = colon + 3;
        // The scheme is the run of scheme characters before the `://`, from
        // its first letter.
        std::size_t scheme = colon;
        while (scheme > copied && is_scheme_char(text[scheme - 1]))
            --scheme;
        while (scheme < colon && !is_alpha(text[scheme]))
            ++scheme;
        if (scheme == colon)
            continue;
        const std::size_t user = colon + 3;
        const std::size_t user_end = skip_while(text, user, is_user_char);
        if (user_end == user || user_end >= text.size() ||
            text[user_end] != ':')
            continue;
        const std::size_t password = user_end + 1;
        const std::size_t password_end =
            skip_while(text, password, is_password_char);
        if (password_end == password || password_end >= text.size() ||
            text[password_end] != '@')
            continue;
        out.append(text, copied, user - copied);
        out += marker;
        copied = password_end;
        search = password_end + 1;
    }
    out.append(text, copied, std::string::npos);
    return out;
}

} // namespace

void redactor::add_literal(std::string needle, std::string replacement)
{
    if (needle.size() < shortest_literal)
        return;
    literals_.push_back({std::move(needle), std::move(replacement)});
    // Longest first, so a folder is replaced before a name inside it.
    std::ranges::stable_sort(literals_, [](const literal& a, const literal& b)
                             { return a.needle.size() > b.needle.size(); });
}

redactor redactor::for_user(std::string_view home, std::string_view user,
                            std::string_view computer)
{
    redactor out;
    out.add_literal(std::string(home), "<home>");
    out.add_literal(std::string(user), "<user>");
    out.add_literal(std::string(computer), "<computer>");
    return out;
}

std::string redactor::apply(std::string_view text) const
{
    std::string out(text);
    for (const literal& item : literals_)
        out = replace_literal(std::move(out), item.needle, item.replacement);
    out = redact_url_passwords(out);
    out = replace_hits(out, named_secret_at);
    out = replace_hits(out, bearer_at);
    out = replace_hits(out, serial_number_at);
    return out;
}

} // namespace vss::debug
