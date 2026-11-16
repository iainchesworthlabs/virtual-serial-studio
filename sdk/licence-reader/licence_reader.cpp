//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: BSD-2-Clause
//
// The reader for Virtual Serial Studio licence files. See licence_reader.hpp
// and README.md. The format is described in docs/licensing.md of Virtual
// Serial Studio; this file was written from that description and shares no
// code with Studio's own library.
//

#include "licence_reader.hpp"

#include <monocypher-ed25519.h>

#include <algorithm>
#include <utility>

namespace vss_licence_reader
{

namespace
{

// -- A small strict JSON reader ---------------------------------------------
//
// It reads RFC 8259 and nothing more: no comments, no trailing commas, one
// value in the whole text. On top of the RFC it refuses a name that appears
// twice in one object, text that is not UTF-8, and nesting deeper than a
// licence needs.

/// An envelope is one object and a payload nests two deep; this leaves room
/// for members a later version adds.
constexpr int max_depth = 8;

enum class json_kind
{
    null,
    boolean,
    /// A number written without a fraction or an exponent that fits in 64
    /// bits with a sign.
    integer,
    /// Any other number.
    number,
    string,
    array,
    object
};

struct json_value
{
    json_kind type = json_kind::null;
    std::int64_t integer = 0;
    std::string text;
    /// The entries of an array, or the values of an object's members.
    std::vector<json_value> items;
    /// The names of an object's members, in step with `items`.
    std::vector<std::string> names;

    const json_value* member(std::string_view name) const
    {
        for (std::size_t index = 0; index < names.size(); ++index)
        {
            if (names[index] == name)
            {
                return &items[index];
            }
        }
        return nullptr;
    }
};

class json_reader
{
public:
    explicit json_reader(std::string_view text) : text_(text) {}

    /// Reads the whole text as one value.
    bool document(json_value& out)
    {
        skip_space();
        if (!read_value(out, 0))
        {
            return false;
        }
        skip_space();
        return at_ == text_.size();
    }

private:
    bool more() const { return at_ < text_.size(); }

    unsigned char peek() const
    {
        return static_cast<unsigned char>(text_[at_]);
    }

    bool take(char expected)
    {
        if (more() && text_[at_] == expected)
        {
            ++at_;
            return true;
        }
        return false;
    }

    void skip_space()
    {
        while (more() && (text_[at_] == ' ' || text_[at_] == '\t' ||
                          text_[at_] == '\n' || text_[at_] == '\r'))
        {
            ++at_;
        }
    }

    bool take_word(std::string_view word)
    {
        if (text_.substr(at_, word.size()) != word)
        {
            return false;
        }
        at_ += word.size();
        return true;
    }

    bool read_value(json_value& out, int depth)
    {
        if (!more())
        {
            return false;
        }
        switch (text_[at_])
        {
        case '{':
            return depth < max_depth && read_object(out, depth);
        case '[':
            return depth < max_depth && read_array(out, depth);
        case '"':
            out.type = json_kind::string;
            return read_string(out.text);
        case 't':
            out.type = json_kind::boolean;
            return take_word("true");
        case 'f':
            out.type = json_kind::boolean;
            return take_word("false");
        case 'n':
            out.type = json_kind::null;
            return take_word("null");
        default:
            return read_number(out);
        }
    }

    bool read_object(json_value& out, int depth)
    {
        out.type = json_kind::object;
        ++at_;
        skip_space();
        if (take('}'))
        {
            return true;
        }
        for (;;)
        {
            std::string name;
            if (!more() || text_[at_] != '"' || !read_string(name))
            {
                return false;
            }
            if (std::find(out.names.begin(), out.names.end(), name) !=
                out.names.end())
            {
                return false;
            }
            skip_space();
            if (!take(':'))
            {
                return false;
            }
            skip_space();
            json_value entry;
            if (!read_value(entry, depth + 1))
            {
                return false;
            }
            out.names.push_back(std::move(name));
            out.items.push_back(std::move(entry));
            skip_space();
            if (take('}'))
            {
                return true;
            }
            if (!take(','))
            {
                return false;
            }
            skip_space();
        }
    }

    bool read_array(json_value& out, int depth)
    {
        out.type = json_kind::array;
        ++at_;
        skip_space();
        if (take(']'))
        {
            return true;
        }
        for (;;)
        {
            json_value entry;
            if (!read_value(entry, depth + 1))
            {
                return false;
            }
            out.items.push_back(std::move(entry));
            skip_space();
            if (take(']'))
            {
                return true;
            }
            if (!take(','))
            {
                return false;
            }
            skip_space();
        }
    }

    bool take_digits()
    {
        const std::size_t start = at_;
        while (more() && text_[at_] >= '0' && text_[at_] <= '9')
        {
            ++at_;
        }
        return at_ > start;
    }

    bool read_number(json_value& out)
    {
        const std::size_t start = at_;
        const bool negative = take('-');
        const std::size_t digits_start = at_;
        if (!take_digits())
        {
            return false;
        }
        // No leading zero: "0" alone, or a first digit that is not zero.
        if (text_[digits_start] == '0' && at_ - digits_start > 1)
        {
            return false;
        }
        const std::size_t digits_end = at_;
        bool whole = true;
        if (take('.'))
        {
            whole = false;
            if (!take_digits())
            {
                return false;
            }
        }
        if (more() && (text_[at_] == 'e' || text_[at_] == 'E'))
        {
            whole = false;
            ++at_;
            if (more() && (text_[at_] == '+' || text_[at_] == '-'))
            {
                ++at_;
            }
            if (!take_digits())
            {
                return false;
            }
        }
        out.type = json_kind::number;
        if (!whole)
        {
            return at_ > start;
        }
        // Count downwards, so the most negative number fits too.
        const std::int64_t lowest = std::numeric_limits<std::int64_t>::min();
        std::int64_t total = 0;
        for (std::size_t index = digits_start; index < digits_end; ++index)
        {
            const std::int64_t digit = text_[index] - '0';
            if (total < (lowest + digit) / 10)
            {
                return true;
            }
            total = total * 10 - digit;
        }
        if (!negative && total == lowest)
        {
            return true;
        }
        out.type = json_kind::integer;
        out.integer = negative ? total : -total;
        return true;
    }

    static int hex_digit(char c)
    {
        if (c >= '0' && c <= '9')
        {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f')
        {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F')
        {
            return c - 'A' + 10;
        }
        return -1;
    }

    bool read_code_unit(unsigned& unit)
    {
        if (text_.size() - at_ < 4)
        {
            return false;
        }
        unit = 0;
        for (int count = 0; count < 4; ++count)
        {
            const int digit = hex_digit(text_[at_++]);
            if (digit < 0)
            {
                return false;
            }
            unit = unit * 16 + static_cast<unsigned>(digit);
        }
        return true;
    }

    static void append_utf8(std::string& out, unsigned point)
    {
        const auto put = [&out](unsigned byte)
        { out.push_back(static_cast<char>(static_cast<unsigned char>(byte))); };
        if (point < 0x80U)
        {
            put(point);
        }
        else if (point < 0x800U)
        {
            put(0xc0U | (point >> 6U));
            put(0x80U | (point & 0x3fU));
        }
        else if (point < 0x10000U)
        {
            put(0xe0U | (point >> 12U));
            put(0x80U | ((point >> 6U) & 0x3fU));
            put(0x80U | (point & 0x3fU));
        }
        else
        {
            put(0xf0U | (point >> 18U));
            put(0x80U | ((point >> 12U) & 0x3fU));
            put(0x80U | ((point >> 6U) & 0x3fU));
            put(0x80U | (point & 0x3fU));
        }
    }

    bool read_escape(std::string& out)
    {
        if (!more())
        {
            return false;
        }
        const char c = text_[at_++];
        switch (c)
        {
        case '"':
        case '\\':
        case '/':
            out.push_back(c);
            return true;
        case 'b':
            out.push_back('\b');
            return true;
        case 'f':
            out.push_back('\f');
            return true;
        case 'n':
            out.push_back('\n');
            return true;
        case 'r':
            out.push_back('\r');
            return true;
        case 't':
            out.push_back('\t');
            return true;
        case 'u':
            break;
        default:
            return false;
        }
        unsigned unit = 0;
        if (!read_code_unit(unit))
        {
            return false;
        }
        if (unit >= 0xdc00U && unit <= 0xdfffU)
        {
            // The second half of a pair with no first half.
            return false;
        }
        if (unit >= 0xd800U && unit <= 0xdbffU)
        {
            unsigned second = 0;
            if (!take('\\') || !take('u') || !read_code_unit(second) ||
                second < 0xdc00U || second > 0xdfffU)
            {
                return false;
            }
            unit = 0x10000U + ((unit - 0xd800U) << 10U) + (second - 0xdc00U);
        }
        append_utf8(out, unit);
        return true;
    }

    /// Takes `count` bytes that continue a UTF-8 sequence. The first of
    /// them must lie between `low` and `high`, which is how the encodings
    /// that are too long, the surrogates and the values past U+10FFFF are
    /// refused.
    bool take_continuation(std::string& out, int count, unsigned low,
                           unsigned high)
    {
        for (int index = 0; index < count; ++index)
        {
            if (!more())
            {
                return false;
            }
            const unsigned byte = peek();
            const unsigned from = index == 0 ? low : 0x80U;
            const unsigned to = index == 0 ? high : 0xbfU;
            if (byte < from || byte > to)
            {
                return false;
            }
            out.push_back(text_[at_++]);
        }
        return true;
    }

    bool read_string(std::string& out)
    {
        ++at_;
        for (;;)
        {
            if (!more())
            {
                return false;
            }
            const unsigned byte = peek();
            if (byte == '"')
            {
                ++at_;
                return true;
            }
            if (byte == '\\')
            {
                ++at_;
                if (!read_escape(out))
                {
                    return false;
                }
                continue;
            }
            if (byte < 0x20U)
            {
                return false;
            }
            out.push_back(text_[at_++]);
            if (byte < 0x80U)
            {
                continue;
            }
            bool good = false;
            if (byte >= 0xc2U && byte <= 0xdfU)
            {
                good = take_continuation(out, 1, 0x80U, 0xbfU);
            }
            else if (byte == 0xe0U)
            {
                good = take_continuation(out, 2, 0xa0U, 0xbfU);
            }
            else if (byte == 0xedU)
            {
                good = take_continuation(out, 2, 0x80U, 0x9fU);
            }
            else if (byte >= 0xe1U && byte <= 0xefU)
            {
                good = take_continuation(out, 2, 0x80U, 0xbfU);
            }
            else if (byte == 0xf0U)
            {
                good = take_continuation(out, 3, 0x90U, 0xbfU);
            }
            else if (byte >= 0xf1U && byte <= 0xf3U)
            {
                good = take_continuation(out, 3, 0x80U, 0xbfU);
            }
            else if (byte == 0xf4U)
            {
                good = take_continuation(out, 3, 0x80U, 0x8fU);
            }
            if (!good)
            {
                return false;
            }
        }
    }

    std::string_view text_;
    std::size_t at_ = 0;
};

// -- Reading members ---------------------------------------------------------

/// A member that must be there: text that is not empty and no longer than
/// `longest`.
bool required_text(const json_value& object, std::string_view name,
                   std::size_t longest, std::string& out)
{
    const json_value* found = object.member(name);
    if (found == nullptr || found->type != json_kind::string ||
        found->text.empty() || found->text.size() > longest)
    {
        return false;
    }
    out = found->text;
    return true;
}

/// A member that may be left out or null, which leaves `out` empty.
bool optional_text(const json_value& object, std::string_view name,
                   std::size_t longest, std::string& out)
{
    out.clear();
    const json_value* found = object.member(name);
    if (found == nullptr || found->type == json_kind::null)
    {
        return true;
    }
    return required_text(object, name, longest, out);
}

bool required_integer(const json_value& object, std::string_view name,
                      std::int64_t lowest, std::int64_t highest,
                      std::int64_t& out)
{
    const json_value* found = object.member(name);
    if (found == nullptr || found->type != json_kind::integer ||
        found->integer < lowest || found->integer > highest)
    {
        return false;
    }
    out = found->integer;
    return true;
}

/// A list of text that may be left out, which leaves `out` empty.
bool text_list(const json_value& object, std::string_view name,
               std::size_t most_entries, std::size_t longest,
               std::vector<std::string>& out)
{
    out.clear();
    const json_value* found = object.member(name);
    if (found == nullptr)
    {
        return true;
    }
    if (found->type != json_kind::array || found->items.size() > most_entries)
    {
        return false;
    }
    for (const json_value& entry : found->items)
    {
        if (entry.type != json_kind::string || entry.text.empty() ||
            entry.text.size() > longest)
        {
            return false;
        }
        out.push_back(entry.text);
    }
    return true;
}

bool is_lower(char c)
{
    return c >= 'a' && c <= 'z';
}

bool is_upper(char c)
{
    return c >= 'A' && c <= 'Z';
}

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

bool is_letter_or_digit(char c)
{
    return is_lower(c) || is_upper(c) || is_digit(c);
}

/// Text made only of characters `allowed` accepts, not empty and no longer
/// than `longest`.
template <typename Allowed>
bool made_of(std::string_view text, std::size_t longest, Allowed allowed)
{
    return !text.empty() && text.size() <= longest &&
           std::all_of(text.begin(), text.end(), allowed);
}

/// Lower-case letters, digits and hyphens: a purpose, a key id, a reset
/// token.
bool is_lower_token(std::string_view text, std::size_t longest)
{
    return made_of(text, longest, [](char c)
                   { return is_lower(c) || is_digit(c) || c == '-'; });
}

/// Upper-case letters, digits and hyphens: a licence id.
bool is_upper_token(std::string_view text, std::size_t longest)
{
    return made_of(text, longest, [](char c)
                   { return is_upper(c) || is_digit(c) || c == '-'; });
}

bool ends_with(std::string_view text, std::string_view ending)
{
    return text.size() >= ending.size() &&
           text.substr(text.size() - ending.size()) == ending;
}

// -- base64url ----------------------------------------------------------------

int base64url_value(char c)
{
    if (is_upper(c))
    {
        return c - 'A';
    }
    if (is_lower(c))
    {
        return c - 'a' + 26;
    }
    if (is_digit(c))
    {
        return c - '0' + 52;
    }
    if (c == '-')
    {
        return 62;
    }
    if (c == '_')
    {
        return 63;
    }
    return -1;
}

/// base64url without padding (RFC 4648, section 5). Padding, white space,
/// the other alphabet and spare bits that are not zero are refused, so each
/// value has one spelling.
bool base64url_decode(std::string_view text, std::vector<std::uint8_t>& out)
{
    out.clear();
    if (text.size() % 4 == 1)
    {
        return false;
    }
    out.reserve(text.size() / 4 * 3 + 2);
    unsigned held = 0;
    unsigned held_bits = 0;
    for (const char c : text)
    {
        const int six = base64url_value(c);
        if (six < 0)
        {
            return false;
        }
        held = ((held << 6U) | static_cast<unsigned>(six)) & 0xfffU;
        held_bits += 6;
        if (held_bits >= 8)
        {
            held_bits -= 8;
            out.push_back(
                static_cast<std::uint8_t>((held >> held_bits) & 0xffU));
        }
    }
    return (held & ((1U << held_bits) - 1U)) == 0;
}

// -- Ed25519 ------------------------------------------------------------------

/// Whether 32 bytes are the one canonical way to write a point (RFC 8032,
/// section 5.1.3). The low 255 bits are y, least significant byte first,
/// and must be below the prime 2^255 - 19. The top bit is the sign of x,
/// which must be clear where x is zero, that is for y = 1 and y = p - 1.
/// Monocypher does the arithmetic; this looks at the bytes only.
bool is_canonical_point(const std::uint8_t* bytes)
{
    std::array<std::uint8_t, 32> y{};
    std::copy(bytes, bytes + 32, y.begin());
    const bool sign = (y[31] & 0x80U) != 0;
    y[31] = static_cast<std::uint8_t>(y[31] & 0x7fU);

    std::array<std::uint8_t, 32> prime{};
    prime.fill(0xff);
    prime[0] = 0xed;
    prime[31] = 0x7f;

    // Compare from the most significant byte down.
    int compared = 0;
    for (std::size_t index = 32; index > 0 && compared == 0; --index)
    {
        if (y[index - 1] != prime[index - 1])
        {
            compared = y[index - 1] < prime[index - 1] ? -1 : 1;
        }
    }
    if (compared >= 0)
    {
        return false;
    }

    std::array<std::uint8_t, 32> one{};
    one[0] = 1;
    std::array<std::uint8_t, 32> minus_one = prime;
    minus_one[0] = 0xec;
    return !(sign && (y == one || y == minus_one));
}

bool signature_holds(const std::vector<std::uint8_t>& signature,
                     const std::array<std::uint8_t, 32>& key,
                     std::string_view purpose,
                     const std::vector<std::uint8_t>& payload)
{
    if (!is_canonical_point(key.data()) ||
        !is_canonical_point(signature.data()))
    {
        return false;
    }
    // What a signature covers: the tag, a zero byte, the purpose, a zero
    // byte, the payload. The purpose is inside, so a signature made for one
    // kind of file never passes as another.
    constexpr std::string_view tag = "VSS-SIGNED-FILE-1";
    std::vector<std::uint8_t> message;
    message.reserve(tag.size() + purpose.size() + payload.size() + 2);
    message.insert(message.end(), tag.begin(), tag.end());
    message.push_back(0);
    message.insert(message.end(), purpose.begin(), purpose.end());
    message.push_back(0);
    message.insert(message.end(), payload.begin(), payload.end());
    return crypto_ed25519_check(signature.data(), key.data(), message.data(),
                                message.size()) == 0;
}

// -- Times --------------------------------------------------------------------

bool is_leap(int year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int days_in_month(int year, int month)
{
    constexpr std::array<int, 12> lengths{31, 28, 31, 30, 31, 30,
                                          31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap(year))
    {
        return 29;
    }
    return lengths[static_cast<std::size_t>(month - 1)];
}

/// Days from 1970-01-01 to a date of the Gregorian calendar, for the years
/// a licence may name (2000 to 9999).
std::int64_t days_since_1970(int year, int month, int day)
{
    // Count from 1 March 2000, so that a leap day is the last day of a
    // year, then move to 1970: 2000-03-01 is day 11017.
    const int shifted_year = month <= 2 ? year - 1 : year;
    const int shifted_month = month <= 2 ? month + 9 : month - 3;
    const std::int64_t years = shifted_year - 2000;
    const std::int64_t leap_days = (years >= 0 ? years : years - 3) / 4 -
                                   (years >= 0 ? years : years - 99) / 100 +
                                   (years >= 0 ? years : years - 399) / 400;
    const std::int64_t month_days = (153 * shifted_month + 2) / 5;
    return 11017 + years * 365 + leap_days + month_days + (day - 1);
}

int two_digits(std::string_view text, std::size_t at)
{
    return (text[at] - '0') * 10 + (text[at + 1] - '0');
}

constexpr seconds day_length = 24 * 60 * 60;

// -- The payload
// ----------------------------------------------------------------

constexpr std::size_t longest_id = 64;
constexpr std::size_t longest_name = 200;
constexpr std::size_t longest_email = 254;
constexpr std::size_t longest_feature = 136;
constexpr std::size_t longest_machine = 64;

/// The payload members this reader understands. A name in `critical` that
/// is not here makes the licence one for a newer version.
constexpr std::array<std::string_view, 15> known_members{
    "licence_id", "kind",          "edition",    "licensee",    "issued_at",
    "starts_at",  "expires_at",    "grace_days", "seats",       "features",
    "machines",   "updates_until", "replaces",   "clock_reset", "critical"};

struct licence_payload
{
    std::string licence_id;
    bool trial = false;
    seconds issued_at = 0;
    seconds starts_at = 0;
    seconds expires_at = 0;
    int grace_days = 0;
    std::vector<std::string> features;
    bool has_updates_until = false;
    seconds updates_until = 0;
    std::string clock_reset;
};

/// A name a person reads: no control characters.
bool is_display_text(std::string_view text)
{
    return std::none_of(text.begin(), text.end(),
                        [](char c)
                        {
                            const auto byte = static_cast<unsigned char>(c);
                            return byte < 0x20U || byte == 0x7fU;
                        });
}

/// The form of an email address a licence may carry: one `@`, something
/// before it made of letters, digits and the marks that need no quoting,
/// and after it a name with a dot that starts and ends with a letter or a
/// digit.
bool is_email_address(std::string_view text)
{
    if (text.empty() || text.size() > longest_email)
    {
        return false;
    }
    const std::size_t at = text.find('@');
    if (at == std::string_view::npos || at == 0 || at != text.rfind('@'))
    {
        return false;
    }
    constexpr std::string_view marks = ".!#$%&'*+/=?^_`{|}~-";
    const std::string_view user = text.substr(0, at);
    const std::string_view host = text.substr(at + 1);
    const bool user_good =
        std::all_of(user.begin(), user.end(),
                    [marks](char c)
                    {
                        return is_letter_or_digit(c) ||
                               marks.find(c) != std::string_view::npos;
                    });
    if (!user_good || host.size() < 3 ||
        host.find('.') == std::string_view::npos ||
        host.find("..") != std::string_view::npos ||
        !is_letter_or_digit(host.front()) || !is_letter_or_digit(host.back()))
    {
        return false;
    }
    return std::all_of(
        host.begin(), host.end(),
        [](char c) { return is_letter_or_digit(c) || c == '.' || c == '-'; });
}

/// A `features` entry: lower-case letters, digits, dots and hyphens, with
/// no dot at either end, and perhaps `.*` after it.
bool is_feature_entry(std::string_view text)
{
    if (ends_with(text, ".*"))
    {
        text.remove_suffix(2);
    }
    const bool form =
        made_of(text, longest_feature, [](char c)
                { return is_lower(c) || is_digit(c) || c == '.' || c == '-'; });
    return form && text.front() != '.' && text.back() != '.';
}

/// A `machines` entry: a computer name, and perhaps `*` after it.
bool is_machine_entry(std::string_view text)
{
    if (ends_with(text, "*"))
    {
        text.remove_suffix(1);
    }
    return made_of(
        text, longest_machine, [](char c)
        { return is_letter_or_digit(c) || c == '-' || c == '_' || c == '.'; });
}

bool required_time(const json_value& object, std::string_view name,
                   seconds& out)
{
    std::string text;
    return required_text(object, name, 20, text) && parse_time(text, out);
}

enum class payload_result
{
    good,
    malformed,
    newer_version
};

/// Reads the signed payload. Every member Studio checks is checked here
/// too, whether or not this reader uses it, so that a file one of them
/// refuses is refused by the other.
payload_result read_payload(const std::vector<std::uint8_t>& bytes,
                            licence_payload& out)
{
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()),
                                bytes.size());
    json_value object;
    if (!json_reader(text).document(object) || object.type != json_kind::object)
    {
        return payload_result::malformed;
    }

    // `critical` first: an old reader must never half understand a new
    // file.
    std::vector<std::string> critical;
    if (!text_list(object, "critical", 64, 64, critical))
    {
        return payload_result::malformed;
    }
    for (const std::string& name : critical)
    {
        if (std::find(known_members.begin(), known_members.end(), name) ==
            known_members.end())
        {
            return payload_result::newer_version;
        }
    }

    std::string kind_text;
    std::string edition_text;
    if (!required_text(object, "licence_id", longest_id, out.licence_id) ||
        !required_text(object, "kind", 16, kind_text) ||
        !required_text(object, "edition", 16, edition_text) ||
        !is_upper_token(out.licence_id, longest_id) ||
        (kind_text != "trial" && kind_text != "annual" &&
         kind_text != "enterprise") ||
        (edition_text != "pro" && edition_text != "enterprise"))
    {
        return payload_result::malformed;
    }
    out.trial = kind_text == "trial";

    const json_value* licensee = object.member("licensee");
    std::string name;
    std::string contact;
    std::string email;
    if (licensee == nullptr || licensee->type != json_kind::object ||
        !required_text(*licensee, "name", longest_name, name) ||
        !optional_text(*licensee, "contact", longest_name, contact) ||
        !optional_text(*licensee, "email", longest_email, email) ||
        !is_display_text(name) || !is_display_text(contact) ||
        (!email.empty() && !is_email_address(email)))
    {
        return payload_result::malformed;
    }

    if (!required_time(object, "issued_at", out.issued_at) ||
        !required_time(object, "starts_at", out.starts_at) ||
        !required_time(object, "expires_at", out.expires_at) ||
        out.expires_at < out.starts_at)
    {
        return payload_result::malformed;
    }

    std::int64_t grace = 0;
    std::int64_t seats = 0;
    if (!required_integer(object, "grace_days", 0, 366, grace) ||
        !required_integer(object, "seats", 0, 1'000'000, seats))
    {
        return payload_result::malformed;
    }
    out.grace_days = static_cast<int>(grace);

    std::vector<std::string> machines;
    if (!text_list(object, "features", 64, longest_feature + 2, out.features) ||
        !text_list(object, "machines", 1024, longest_machine + 1, machines) ||
        !std::all_of(out.features.begin(), out.features.end(),
                     [](const std::string& entry)
                     { return is_feature_entry(entry); }) ||
        !std::all_of(machines.begin(), machines.end(),
                     [](const std::string& entry)
                     { return is_machine_entry(entry); }))
    {
        return payload_result::malformed;
    }

    std::string updates;
    std::string replaces;
    if (!optional_text(object, "updates_until", 20, updates) ||
        !optional_text(object, "replaces", longest_id, replaces) ||
        !optional_text(object, "clock_reset", longest_id, out.clock_reset) ||
        (!replaces.empty() && !is_upper_token(replaces, longest_id)) ||
        (!out.clock_reset.empty() &&
         !is_lower_token(out.clock_reset, longest_id)))
    {
        return payload_result::malformed;
    }
    if (!updates.empty())
    {
        if (!parse_time(updates, out.updates_until))
        {
            return payload_result::malformed;
        }
        out.has_updates_until = true;
    }
    return payload_result::good;
}

// -- The envelope
// ---------------------------------------------------------------

struct envelope
{
    std::string purpose;
    std::string key_id;
    std::vector<std::uint8_t> payload;
    std::vector<std::uint8_t> signature;
};

/// Reads the envelope: the size, the JSON, the five members and the
/// base64url. `reason` is set when it returns false.
bool read_envelope(std::string_view text, envelope& out, std::string& reason)
{
    reason = "malformed";
    if (text.size() > max_file_size)
    {
        return false;
    }
    json_value object;
    if (!json_reader(text).document(object) || object.type != json_kind::object)
    {
        return false;
    }
    // The format comes first: a later format may have other members.
    std::int64_t format = 0;
    if (!required_integer(object, "format", 1, 1000, format))
    {
        return false;
    }
    if (format != 1)
    {
        reason = "newer_version";
        return false;
    }
    std::string payload_text;
    std::string signature_text;
    if (object.names.size() != 5 ||
        !required_text(object, "purpose", 32, out.purpose) ||
        !required_text(object, "key_id", 64, out.key_id) ||
        !required_text(object, "payload", max_file_size, payload_text) ||
        !required_text(object, "signature", 86, signature_text) ||
        !is_lower_token(out.purpose, 32) || !is_lower_token(out.key_id, 64))
    {
        return false;
    }
    return base64url_decode(payload_text, out.payload) &&
           base64url_decode(signature_text, out.signature) &&
           out.signature.size() == 64;
}

bool names_feature(const licence_payload& licence,
                   const std::vector<std::string>& asked)
{
    for (const std::string& feature : asked)
    {
        for (const std::string& entry : licence.features)
        {
            if (feature_matches(entry, feature))
            {
                return true;
            }
        }
    }
    return false;
}

} // namespace

bool parse_time(std::string_view text, seconds& value)
{
    // 2026-11-13T02:10:00Z
    constexpr std::string_view shape = "dddd-dd-ddTdd:dd:ddZ";
    if (text.size() != shape.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < shape.size(); ++index)
    {
        const bool fits = shape[index] == 'd' ? is_digit(text[index])
                                              : text[index] == shape[index];
        if (!fits)
        {
            return false;
        }
    }
    const int year = two_digits(text, 0) * 100 + two_digits(text, 2);
    const int month = two_digits(text, 5);
    const int day = two_digits(text, 8);
    const int hour = two_digits(text, 11);
    const int minute = two_digits(text, 14);
    const int second = two_digits(text, 17);
    if (year < 2000 || month < 1 || month > 12 || day < 1 ||
        day > days_in_month(year, month) || hour > 23 || minute > 59 ||
        second > 59)
    {
        return false;
    }
    value = days_since_1970(year, month, day) * day_length + hour * 3600 +
            minute * 60 + second;
    return true;
}

bool feature_matches(std::string_view entry, std::string_view feature)
{
    if (ends_with(entry, ".*"))
    {
        // "plugins.*" covers "plugins.x" and not "plugins" or "pluginsx".
        entry.remove_suffix(1);
        return feature.size() > entry.size() &&
               feature.substr(0, entry.size()) == entry;
    }
    return entry == feature;
}

answer read_licence(std::string_view file_text, const question& asked)
{
    answer result;

    envelope file;
    if (!read_envelope(file_text, file, result.reason))
    {
        return result;
    }
    if (file.purpose != "licence")
    {
        result.reason = "malformed";
        return result;
    }
    const auto key = std::find_if(asked.keys.begin(), asked.keys.end(),
                                  [&file](const trusted_key& candidate)
                                  { return candidate.id == file.key_id; });
    if (key == asked.keys.end())
    {
        // A key this program does not know: a newer program may.
        result.reason = "newer_version";
        return result;
    }
    if (!signature_holds(file.signature, key->public_key, file.purpose,
                         file.payload))
    {
        result.reason = "bad_signature";
        return result;
    }

    licence_payload licence;
    const payload_result outcome = read_payload(file.payload, licence);
    if (outcome != payload_result::good)
    {
        result.reason = outcome == payload_result::newer_version
                            ? "newer_version"
                            : "malformed";
        return result;
    }

    result.verified = true;
    result.licence_id = licence.licence_id;
    result.key_id = file.key_id;
    result.issued_at = licence.issued_at;
    result.starts_at = licence.starts_at;
    result.expires_at = licence.expires_at;
    result.grace_ends = licence.expires_at + licence.grace_days * day_length;
    result.clock_reset = licence.clock_reset;

    // The key may sign only for the times in its window.
    if (licence.issued_at < key->not_before ||
        licence.issued_at > key->not_after)
    {
        result.reason = "revoked";
        return result;
    }

    // Times that have certainly passed. The clock may not read well before
    // the latest of them.
    const seconds passed =
        std::max({asked.latest_seen, asked.build_time, licence.issued_at});
    if (asked.now < passed - clock_tolerance)
    {
        result.licence_state = state::clock_suspect;
        result.reason = "clock_suspect";
        return result;
    }
    if (asked.now < licence.starts_at)
    {
        result.licence_state = state::not_yet_valid;
        result.reason = "not_yet_valid";
        return result;
    }

    const bool named = names_feature(licence, asked.features);
    if (asked.now > result.grace_ends)
    {
        result.licence_state = state::expired;
        result.reason = "expired";
        // The version window: what was released while the licence ran
        // still works, if the licence names it.
        if (named && asked.has_released && licence.has_updates_until &&
            asked.released <= licence.updates_until)
        {
            result.granted = true;
            result.reason = "ok";
            result.by_version_window = true;
        }
        return result;
    }

    if (asked.now <= licence.expires_at)
    {
        result.days_left =
            static_cast<int>((licence.expires_at - asked.now) / day_length);
        const int threshold =
            licence.trial ? expiring_days_trial : expiring_days;
        result.licence_state =
            result.days_left <= threshold ? state::expiring : state::valid;
    }
    else
    {
        result.days_left =
            static_cast<int>((result.grace_ends - asked.now) / day_length);
        result.licence_state = state::grace;
    }
    result.granted = asked.features.empty() || named;
    result.reason = result.granted ? "ok" : "not_in_licence";
    return result;
}

clock_state observe(clock_state stored, seconds now)
{
    stored.latest_seen = std::max(stored.latest_seen, now);
    return stored;
}

clock_state apply_reset(clock_state stored, const answer& verified_licence)
{
    constexpr std::size_t most_kept = 32;
    const std::string& token = verified_licence.clock_reset;
    if (!verified_licence.verified || token.empty() ||
        std::find(stored.applied_resets.begin(), stored.applied_resets.end(),
                  token) != stored.applied_resets.end())
    {
        return stored;
    }
    stored.latest_seen = verified_licence.issued_at;
    stored.applied_resets.push_back(token);
    if (stored.applied_resets.size() > most_kept)
    {
        stored.applied_resets.erase(stored.applied_resets.begin());
    }
    return stored;
}

} // namespace vss_licence_reader
