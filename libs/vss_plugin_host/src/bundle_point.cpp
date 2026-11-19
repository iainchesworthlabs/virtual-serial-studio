//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/host/bundle_point.hpp>

#include <vss/plugin/api_capture.hpp>
#include <vss/plugin/api_message.hpp>
#include <vss/plugin/host/bundle_sections.hpp>
#include <vss/plugin/host/extension.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/protocol.hpp>

#include <boost/asio/post.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vss::plugin::host
{

namespace
{

using namespace std::chrono_literals;

/// The number a request is encoded with; the runner gives it its own.
constexpr std::uint64_t placeholder_id = 1;
constexpr auto prepare_timeout = 30'000ms;
/// How many sections one plugin may be asked for at the same time.
constexpr std::size_t most_pending = 8;

bundle_outcome refused(std::string reason,
                       std::optional<bundle_failure> failure = std::nullopt)
{
    bundle_outcome out;
    out.reason = std::move(reason);
    out.failure = std::move(failure);
    return out;
}

/// Reads the files of a section and checks them.  Runs on the slow
/// executor and touches nothing of the handler.
bundle_outcome read_section(const std::filesystem::path& data_directory,
                            const section_rules& rules, const bundle_ask& ask,
                            const std::vector<bundle_file>& files)
{
    offered_section offered;
    offered.id = ask.section;
    offered.title = ask.title;
    offered.sensitive = ask.sensitive;
    for (const bundle_file& file : files)
    {
        std::string path = "bundles/";
        path.append(ask.section);
        path.push_back('/');
        path.append(file.name);
        const auto measured =
            measure_file(data_directory, ask.section, path, file.size);
        if (!measured)
        {
            return refused("a file of the section could not be read as it was "
                           "listed",
                           measured.error());
        }
        offered_file listed;
        listed.path = std::move(path);
        listed.size = measured->size;
        listed.sha512 = measured->sha512;
        offered.files.push_back(std::move(listed));
    }
    const std::array<offered_section, 1> list{offered};
    auto verified = verify_sections(data_directory, list, rules);
    if (!verified)
    {
        return refused("the section's files did not pass the check",
                       verified.error());
    }
    bundle_outcome out;
    bundle_prepared prepared;
    prepared.offered = std::move(offered);
    prepared.verified = std::move(verified.value().front());
    out.prepared = std::move(prepared);
    return out;
}

class bundle_point_impl final : public bundle_point
{
public:
    bundle_point_impl(extension_context context,
                      boost::asio::any_io_executor slow,
                      std::vector<std::string> reserved_ids)
        : context_(std::move(context)), slow_(std::move(slow)),
          alive_(std::make_shared<bool>(true))
    {
        rules_.plugin_id = context_.plugin_id;
        rules_.reserved_ids = std::move(reserved_ids);
    }

    ~bundle_point_impl() override
    {
        *alive_ = false;
        complete_all("the plugin's host is closing");
    }

    bool take(const api_message&) override { return false; }

    void plugin_gone() override { complete_all("the plugin has stopped"); }

    void prepare(bundle_ask ask, bundle_done done) override
    {
        if (!context_.may(permission::bundle_section))
        {
            done(refused("the plugin may not add a section to a bundle"));
            return;
        }
        offered_section probe;
        probe.id = ask.section;
        probe.title = ask.title;
        probe.sensitive = ask.sensitive;
        const std::array<offered_section, 1> probes{probe};
        if (auto found = check_sections(probes, rules_))
        {
            done(refused("the section is not one the plugin may offer",
                         std::move(found)));
            return;
        }
        if (pending_.size() >= most_pending)
        {
            done(refused("the plugin is already asked for too many sections"));
            return;
        }
        bundle_prepare_message request;
        request.section = ask.section;
        request.settings = ask.settings;
        auto encoded = encode_as(request, placeholder_id);
        if (!encoded)
        {
            done(refused("the section could not be asked for"));
            return;
        }
        const std::uint64_t token = ++tokens_;
        pending_.emplace(token, std::move(done));
        context_.link.request(
            std::move(*encoded),
            [alive = alive_, this, token, ask = std::move(ask)](
                error_code failure, const api_message& reply)
            {
                if (*alive)
                    answered(token, ask, failure, reply);
            },
            prepare_timeout);
    }

private:
    void answered(std::uint64_t token, const bundle_ask& ask,
                  error_code failure, const api_message& reply)
    {
        if (!pending_.contains(token))
            return;
        if (failure)
        {
            complete(token, refused("the plugin did not prepare the section"));
            return;
        }
        auto result = decode_as<bundle_result_message>(reply);
        if (!result)
        {
            complete(token,
                     refused("the plugin's answer is not a list of files"));
            return;
        }
        boost::asio::post(
            slow_,
            [alive = alive_, executor = context_.executor, this, token,
             directory = context_.data_directory, rules = rules_, ask,
             files = std::move(result->files)]()
            {
                bundle_outcome out = read_section(directory, rules, ask, files);
                boost::asio::post(
                    executor,
                    [alive, this, token, out = std::move(out)]() mutable
                    {
                        if (*alive)
                            complete(token, std::move(out));
                    });
            });
    }

    /// Calls the handler of a request, once.
    void complete(std::uint64_t token, bundle_outcome outcome)
    {
        const auto found = pending_.find(token);
        if (found == pending_.end())
            return;
        bundle_done done = std::move(found->second);
        pending_.erase(found);
        done(std::move(outcome));
    }

    void complete_all(const std::string& why)
    {
        auto waiting = std::move(pending_);
        pending_.clear();
        for (auto& [token, done] : waiting)
            done(refused(why));
    }

    extension_context context_;
    boost::asio::any_io_executor slow_;
    section_rules rules_;
    /// False once this object has gone.  Read and written on the service's
    /// strand only.
    std::shared_ptr<bool> alive_;
    std::map<std::uint64_t, bundle_done> pending_;
    std::uint64_t tokens_ = 0;
};

} // namespace

std::unique_ptr<bundle_point>
make_bundle_point(const extension_context& context,
                  boost::asio::any_io_executor slow,
                  std::vector<std::string> reserved_ids)
{
    return std::make_unique<bundle_point_impl>(context, std::move(slow),
                                               std::move(reserved_ids));
}

} // namespace vss::plugin::host
