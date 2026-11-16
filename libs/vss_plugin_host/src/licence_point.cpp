//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/host/licence_point.hpp>

#include <vss/core/bytes.hpp>
#include <vss/plugin/api_licence.hpp>
#include <vss/plugin/api_message.hpp>
#include <vss/plugin/host/extension.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/protocol.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace vss::plugin::host
{

namespace
{

class licence_point final : public extension_handler
{
public:
    licence_point(extension_context context, licence_file_source source)
        : context_(std::move(context)), source_(std::move(source))
    {
    }

    bool take(const api_message& message) override
    {
        if (message.op == licence_get_message::op())
        {
            get(message);
            return true;
        }
        if (message.op == licence_report_message::op())
        {
            report(message);
            return true;
        }
        return false;
    }

private:
    void refuse(std::uint64_t id, api_errc code, const std::string& text)
    {
        if (id != 0)
            context_.link.answer(id, error_answer(code, text));
    }

    void get(const api_message& message)
    {
        if (!decode_as<licence_get_message>(message))
        {
            refuse(message.id, api_errc::bad_request,
                   "the licence was not asked for as it must be");
            return;
        }
        // The file holds the user's name, so nobody but an official plugin
        // has it without the user's say.
        if (!context_.official && !context_.may(permission::licence_read))
        {
            refuse(message.id, api_errc::not_granted,
                   "the plugin may not read the licence file");
            return;
        }
        licence_file_message body;
        if (source_)
        {
            std::optional<byte_buffer> bytes;
            try
            {
                bytes = source_();
            }
            catch (...)
            {
                // A source that fails has no licence to give.
                bytes.reset();
            }
            if (bytes && !bytes->empty() && bytes->size() <= max_licence_file)
            {
                body.present = true;
                body.data = std::move(*bytes);
            }
        }
        auto answer = encode_as(body, message.id);
        if (!answer)
        {
            refuse(message.id, api_errc::refused,
                   "the licence file could not be sent");
            return;
        }
        context_.link.answer(message.id, std::move(*answer));
    }

    void report(const api_message& message)
    {
        const auto said = decode_as<licence_report_message>(message);
        if (!said)
        {
            // A report that asks for an answer is told it was not taken.
            refuse(message.id, api_errc::bad_request,
                   "the report was not written as it must be");
            return;
        }
        if (context_.licence_reported)
        {
            context_.licence_reported(
                plugin_licence_report{said->reason, said->licence_id});
        }
    }

    extension_context context_;
    licence_file_source source_;
};

} // namespace

std::unique_ptr<extension_handler>
make_licence_point(const extension_context& context, licence_file_source source)
{
    return std::make_unique<licence_point>(context, std::move(source));
}

} // namespace vss::plugin::host
