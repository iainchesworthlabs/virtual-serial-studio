//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The tokens of a gateway: making one, checking one, revoking one.  What is
// on disk is in token_file.cpp; this file is the rules.
//
// The rule that matters most is in `verify`: whatever is wrong with what was
// presented, one secret is hashed and one digest is compared in constant
// time, so the work done does not say whether the id exists, whether the
// token was revoked or whether only the secret was wrong.
//

#include <vss/control/token_store.hpp>

#include "platform/private_storage_platform.hpp"
#include "token_file.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <span>
#include <utility>

namespace vss::control
{
namespace
{

/// How often a record's `last_used` is written to the file.  It is the only
/// thing a successful check writes, and it need not be exact.
constexpr std::chrono::minutes last_used_resolution{1};
/// A record is dropped this long after its token ended.
constexpr std::chrono::hours kept_after_expiry{24 * 7};

/// What a presented secret is compared with when there is no record: a
/// digest no secret hashes to in practice.  It only has to cost a compare.
constexpr token_digest dummy_digest{};

/// True when `label` is text a list can show: valid UTF-8 without control
/// characters, and not too long.
bool acceptable_label(std::string_view label) noexcept
{
    if (label.size() > max_token_label_size)
        return false;
    std::size_t index = 0;
    while (index < label.size())
    {
        const auto lead = static_cast<unsigned char>(label[index]);
        std::size_t length = 0;
        if (lead < 0x20 || lead == 0x7f)
            return false;
        if (lead < 0x80)
            length = 1;
        else if ((lead & 0xe0) == 0xc0 && lead >= 0xc2)
            length = 2;
        else if ((lead & 0xf0) == 0xe0)
            length = 3;
        else if ((lead & 0xf8) == 0xf0 && lead <= 0xf4)
            length = 4;
        else
            return false;
        if (label.size() - index < length)
            return false;
        for (std::size_t offset = 1; offset < length; ++offset)
        {
            const auto next = static_cast<unsigned char>(label[index + offset]);
            if ((next & 0xc0) != 0x80)
                return false;
        }
        index += length;
    }
    return true;
}

bool allows(const token_record& record, transport via) noexcept
{
    return std::ranges::find(record.transports, via) != record.transports.end();
}

} // namespace

std::string_view token_check_name(token_check value) noexcept
{
    switch (value)
    {
    case token_check::accepted:
        return "accepted";
    case token_check::malformed:
        return "malformed";
    case token_check::unknown:
        return "unknown";
    case token_check::wrong_secret:
        return "wrong_secret";
    case token_check::revoked:
        return "revoked";
    case token_check::expired:
        return "expired";
    case token_check::wrong_transport:
        return "wrong_transport";
    }
    return "malformed";
}

class token_store::impl
{
public:
    impl(std::filesystem::path file, time_source now, token_file_content read)
        : file_(std::move(file)), source_(std::move(now)),
          content_(std::move(read)), saved_latest_(content_.latest_seen)
    {
        if (!source_)
            source_ = [] { return wall_clock::now(); };
    }

    result<new_token> create(const token_request& request)
    {
        if (request.scopes.empty() || request.scopes.contains(scope::admin) ||
            request.transports.empty() ||
            request.life <= std::chrono::seconds::zero() ||
            request.life > longest_token_life ||
            !acceptable_label(request.label))
            return make_error_code(errc::invalid_argument);
        // A token is for a transport a caller can come over; the
        // administration endpoint takes none.
        if (std::ranges::find(request.transports, transport::local_admin) !=
            request.transports.end())
            return make_error_code(errc::invalid_argument);

        const std::lock_guard lock(mutex_);
        const auto now = current_time();
        prune(now);
        if (content_.tokens.size() >= max_tokens)
            return make_error_code(errc::queue_full);

        std::array<std::uint8_t, token_id_bytes + token_secret_bytes> random{};
        if (const error_code failed = platform::random_bytes(random))
            return failed;
        const std::span<const std::uint8_t> bytes(random);
        token_record record;
        record.id = base32_encode(bytes_view(
            reinterpret_cast<const byte*>(bytes.data()), token_id_bytes));
        // Two tokens with one id cannot both be kept.  With 64 random bits
        // this does not happen; if it does, the caller asks again.
        if (find(record.id) != nullptr)
            return make_error_code(errc::already_exists);

        std::string secret_text = base32_encode(bytes_view(
            reinterpret_cast<const byte*>(bytes.data()) + token_id_bytes,
            token_secret_bytes));
        const secret_string secret(secret_text);
        secure_wipe(secret_text.data(), secret_text.size());
        secure_wipe(reinterpret_cast<char*>(random.data()), random.size());

        record.label = request.label;
        record.scopes = request.scopes;
        record.transports = request.transports;
        record.created = now;
        record.expires = now + request.life;
        record.hash = hash_token_secret(secret);

        content_.tokens.push_back(record);
        if (const error_code failed = save(now))
        {
            // A token whose record is not on disk would stop working at the
            // next start, so it is not handed out.
            content_.tokens.pop_back();
            return failed;
        }
        new_token out;
        out.token = make_token(record.id, secret);
        out.record = std::move(record);
        return out;
    }

    token_verdict verify(const secret_string& presented, transport via)
    {
        token_verdict verdict;
        auto parts = parse_token(presented);

        const std::lock_guard lock(mutex_);
        const auto now = current_time();
        const token_record* const record = parts ? find(parts->id) : nullptr;

        // The same work on every path: one hash, one compare.  Something
        // that is not a token is hashed as it is.
        const token_digest digest =
            hash_token_secret(parts ? parts->secret : presented);
        hashes_.fetch_add(1);
        const bool matches = same_digest(
            digest, record != nullptr ? record->hash : dummy_digest);

        if (!parts)
        {
            verdict.outcome = token_check::malformed;
            return verdict;
        }
        verdict.id = parts->id;
        if (record == nullptr)
            verdict.outcome = token_check::unknown;
        else if (!matches)
            verdict.outcome = token_check::wrong_secret;
        else if (record->revoked)
            verdict.outcome = token_check::revoked;
        else if (now >= record->expires)
        {
            verdict.outcome = token_check::expired;
            remember_time();
        }
        else if (!allows(*record, via))
            verdict.outcome = token_check::wrong_transport;
        else
        {
            verdict.outcome = token_check::accepted;
            verdict.scopes = record->scopes;
            verdict.expires = record->expires;
            used(parts->id, now);
        }
        return verdict;
    }

    token_check standing(std::string_view id)
    {
        const std::lock_guard lock(mutex_);
        const token_record* const record = find(id);
        if (record == nullptr)
            return token_check::unknown;
        if (record->revoked)
            return token_check::revoked;
        if (current_time() >= record->expires)
        {
            remember_time();
            return token_check::expired;
        }
        return token_check::accepted;
    }

    result<void> revoke(std::string_view id)
    {
        const std::lock_guard lock(mutex_);
        token_record* const record = find(id);
        if (record == nullptr)
            return make_error_code(errc::not_found);
        if (record->revoked)
            return {};
        record->revoked = true;
        if (const error_code failed = save(current_time()))
        {
            // Revoked for as long as this gateway runs, even if the file
            // could not be written; the caller is told it did not last.
            return failed;
        }
        return {};
    }

    std::vector<token_record> list()
    {
        const std::lock_guard lock(mutex_);
        prune(current_time());
        return content_.tokens;
    }

    std::uint64_t hashes_computed() const noexcept { return hashes_.load(); }

private:
    /// The time, never earlier than the latest this store has seen.  The
    /// latest time is written to the file when it has moved on by a minute,
    /// so that a restart with the clock set back cannot lose more than that.
    /// A write that fails costs only that: the time in memory still holds.
    wall_clock::time_point current_time()
    {
        const auto now = source_();
        if (now > content_.latest_seen)
            content_.latest_seen = now;
        if (content_.latest_seen - saved_latest_ >= last_used_resolution)
            static_cast<void>(save(content_.latest_seen));
        return content_.latest_seen;
    }

    /// Writes the latest time to the file now.  Called when a token was
    /// found to have ended, so that the time that ended it is on disk and a
    /// restart with the clock set back cannot bring the token back, not even
    /// for the minute the periodic write allows.
    void remember_time()
    {
        if (content_.latest_seen > saved_latest_)
            static_cast<void>(save(content_.latest_seen));
    }

    token_record* find(std::string_view id) noexcept
    {
        const auto found =
            std::ranges::find(content_.tokens, id, &token_record::id);
        return found == content_.tokens.end() ? nullptr : &*found;
    }

    /// Drops the records of tokens that ended long enough ago.
    void prune(wall_clock::time_point now)
    {
        std::erase_if(content_.tokens, [now](const token_record& record)
                      { return now >= record.expires + kept_after_expiry; });
    }

    /// Notes a use.  The file is written at most once a minute for it, and
    /// a write that fails costs only the note.
    void used(std::string_view id, wall_clock::time_point now)
    {
        token_record* const record = find(id);
        if (record == nullptr)
            return;
        const bool stale = !record->last_used ||
                           now - *record->last_used >= last_used_resolution;
        if (!stale)
            return;
        record->last_used = now;
        static_cast<void>(save(now));
    }

    error_code save(wall_clock::time_point now)
    {
        content_.latest_seen = now;
        const error_code failed = save_token_file(file_, content_);
        if (!failed)
            saved_latest_ = now;
        return failed;
    }

    const std::filesystem::path file_;
    time_source source_;
    std::mutex mutex_;
    token_file_content content_;
    /// The latest time that is in the file.
    wall_clock::time_point saved_latest_;
    std::atomic<std::uint64_t> hashes_{0};
};

token_store::token_store(std::unique_ptr<impl> state) : impl_(std::move(state))
{
}

token_store::~token_store() = default;

result<std::unique_ptr<token_store>>
token_store::open(std::filesystem::path file, time_source now)
{
    if (file.empty() || !file.has_parent_path())
        return make_error_code(errc::invalid_argument);
    if (const error_code failed =
            platform::ensure_private_directory(file.parent_path()))
        return failed;
    auto read = load_token_file(file);
    if (!read)
        return read.error();
    return std::unique_ptr<token_store>(new token_store(std::make_unique<impl>(
        std::move(file), std::move(now), std::move(*read))));
}

result<new_token> token_store::create(const token_request& request)
{
    return impl_->create(request);
}

token_verdict token_store::verify(const secret_string& presented, transport via)
{
    return impl_->verify(presented, via);
}

token_check token_store::standing(std::string_view id)
{
    return impl_->standing(id);
}

result<void> token_store::revoke(std::string_view id)
{
    return impl_->revoke(id);
}

std::vector<token_record> token_store::list()
{
    return impl_->list();
}

std::uint64_t token_store::hashes_computed() const noexcept
{
    return impl_->hashes_computed();
}

} // namespace vss::control
