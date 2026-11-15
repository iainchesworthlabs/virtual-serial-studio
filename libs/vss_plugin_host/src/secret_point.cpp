//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/host/secret_point.hpp>

#include <vss/plugin/host/control_relay.hpp>
#include <vss/plugin/host/extension.hpp>
#include <vss/plugin/protocol.hpp>

#include <boost/asio/post.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace vss::plugin::host
{

namespace
{

namespace json = boost::json;

constexpr std::string_view show_op = "secret.show";
constexpr std::size_t longest_label = 64;
constexpr std::size_t longest_secret = 512;
constexpr std::size_t longest_name = 128;
constexpr std::int64_t longest_life_s = 3600;

/// A text member of 1 to `most` bytes, or nothing.
std::optional<std::string> text_of(const json::object& header,
                                   std::string_view key, std::size_t most)
{
    const json::value* const found = header.if_contains(key);
    if (found == nullptr || !found->is_string())
        return std::nullopt;
    const json::string& text = found->get_string();
    if (text.empty() || text.size() > most)
        return std::nullopt;
    return std::string(text.data(), text.size());
}

/// What the plugin asked, read strictly; nothing when it is not a
/// "secret.show" as the header of this file describes it.
std::optional<secret_to_show> read(const api_message& message)
{
    const auto* const other = std::get_if<other_message>(&message.body);
    if (other == nullptr)
        return std::nullopt;
    boost::system::error_code failed;
    const json::value parsed = json::parse(other->header, failed);
    if (failed || !parsed.is_object())
        return std::nullopt;
    const json::object& header = parsed.get_object();

    secret_to_show secret;
    auto label = text_of(header, "label", longest_label);
    auto text = text_of(header, "text", longest_secret);
    const json::value* const life = header.if_contains("expires_s");
    if (!label || !text || life == nullptr || !life->is_int64())
        return std::nullopt;
    const std::int64_t seconds = life->get_int64();
    if (seconds < 1 || seconds > longest_life_s)
        return std::nullopt;
    // The secret is shown exactly as it is, so it may hold only what
    // every program shows the same way: the printable characters of
    // ASCII.  Anything else (a control character, a character that
    // changes the direction of text, one that cannot be seen) could only
    // make the user read something other than what was sent.
    const bool plain =
        std::ranges::all_of(*text,
                            [](char c)
                            {
                                const auto byte = static_cast<unsigned char>(c);
                                return byte >= 0x20 && byte <= 0x7e;
                            });
    if (!plain)
        return std::nullopt;
    secret.label = clean_control_text(*label, longest_label);
    if (secret.label.empty())
        return std::nullopt;
    secret.text = std::move(*text);
    secret.expires = std::chrono::seconds{seconds};
    return secret;
}

class secret_point final : public extension_handler
{
public:
    secret_point(extension_context context, secret_shower show)
        : context_(std::move(context)), show_(std::move(show)),
          alive_(std::make_shared<state>())
    {
    }

    ~secret_point() override { alive_->gone = true; }

    bool take(const api_message& message) override
    {
        if (message.op != show_op)
            return false;
        // A secret that wants no answer cannot be told it was not shown.
        if (message.id == 0)
            return true;
        auto secret = read(message);
        if (!secret)
        {
            refuse(message.id, api_errc::bad_request,
                   "the secret could not be read");
            return true;
        }
        if (!show_)
        {
            refuse(message.id, api_errc::not_granted,
                   "this program cannot show a secret");
            return true;
        }
        if (alive_->showing)
        {
            refuse(message.id, api_errc::busy, "a secret is being shown");
            return true;
        }
        secret->plugin_id = context_.plugin_id;
        secret->plugin_name =
            clean_control_text(context_.plugin_name, longest_name);
        alive_->showing = true;
        const std::uint64_t epoch = alive_->epoch;
        show_(std::move(*secret),
              [alive = alive_, link = context_.link,
               executor = context_.executor, epoch, id = message.id](bool seen)
              {
                  // The answer goes back on the service's strand, and only
                  // to the plugin that asked.
                  boost::asio::post(
                      executor,
                      [alive, link, epoch, id, seen]()
                      {
                          if (alive->gone || alive->epoch != epoch)
                              return;
                          alive->showing = false;
                          link.answer(id,
                                      seen ? ok_answer()
                                           : error_answer(api_errc::not_granted,
                                                          "the secret was not "
                                                          "shown"));
                      });
              });
        return true;
    }

    void plugin_gone() override
    {
        // An answer that comes later belongs to a plugin that has gone.
        ++alive_->epoch;
        alive_->showing = false;
    }

private:
    /// What a late answer needs to find out that it is late.  Changed on
    /// the service's strand only.
    struct state
    {
        bool gone = false;
        bool showing = false;
        std::uint64_t epoch = 0;
    };

    void refuse(std::uint64_t id, api_errc code, const std::string& text)
    {
        context_.link.answer(id, error_answer(code, text));
    }

    extension_context context_;
    secret_shower show_;
    std::shared_ptr<state> alive_;
};

} // namespace

std::unique_ptr<extension_handler>
make_secret_point(const extension_context& context, secret_shower show)
{
    return std::make_unique<secret_point>(context, std::move(show));
}

} // namespace vss::plugin::host
