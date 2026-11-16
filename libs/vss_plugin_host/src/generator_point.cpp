//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/host/generator_point.hpp>

#include <vss/core/bytes.hpp>
#include <vss/core/endpoint.hpp>
#include <vss/plugin/api_generator.hpp>
#include <vss/plugin/api_message.hpp>
#include <vss/plugin/host/extension.hpp>
#include <vss/plugin/host/generator_feed.hpp>
#include <vss/plugin/host/instances.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/protocol.hpp>

#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace vss::plugin::host
{

namespace
{

/// The number a request is written with.  The session gives every request
/// its real number when it is sent.
constexpr std::uint64_t placeholder_id = 1;

/// How long the plugin has to answer `generator.start`.
constexpr std::chrono::milliseconds start_timeout{10000};

/// How long it has to answer `generator.stop`.
constexpr std::chrono::milliseconds stop_timeout{3000};

/// The name of the parameter that names the port.
constexpr std::string_view port_parameter = "port";

/// Parameters as a message carries them: the same JSON with no spaces.
std::string as_sent(const std::string& params)
{
    boost::system::error_code failed;
    const boost::json::value parsed = boost::json::parse(params, failed);
    if (failed || !parsed.is_object())
        return params;
    return boost::json::serialize(parsed);
}

/// The port a set of parameters names, or nothing.
std::optional<std::string> port_named(const std::string& params)
{
    boost::system::error_code failed;
    const boost::json::value parsed = boost::json::parse(params, failed);
    if (failed || !parsed.is_object())
        return std::nullopt;
    const boost::json::object& object = parsed.as_object();
    const auto found = object.find(port_parameter);
    if (found == object.end() || !found->value().is_string())
        return std::nullopt;
    return std::string(found->value().as_string());
}

class generator_handler final : public generator_point
{
public:
    generator_handler(extension_context context, generator_owner owner)
        : context_(std::move(context)), owner_(std::move(owner)),
          alive_(std::make_shared<bool>(true))
    {
    }

    ~generator_handler() override
    {
        *alive_ = false;
        try
        {
            stop_all();
        }
        catch (...)
        {
            // Nothing more can be done for a generator that cannot be
            // stopped; the feeds go with the object.
            runs_.clear();
        }
    }

    bool take(const api_message& message) override
    {
        if (message.op == generator_write_message::op())
        {
            write(message);
            return true;
        }
        if (message.op == generator_done_message::op())
        {
            done(message);
            return true;
        }
        if (message.op == generator_pattern_message::op())
        {
            pattern(message);
            return true;
        }
        if (message.op == generator_start_message::op() ||
            message.op == generator_credit_message::op() ||
            message.op == generator_stop_message::op())
        {
            // These are the host's to send.
            ++counts_.wrong_side;
            refuse(message.id, api_errc::bad_request,
                   "that message is the host's to send");
            return true;
        }
        return false;
    }

    void plugin_ready() override { sync(); }

    void plugin_gone() override { stop_all(); }

    void sync() override
    {
        if (!context_.instances)
            return;
        // A generator whose instance is not running is stopped.
        std::vector<std::uint64_t> gone;
        for (const auto& entry : runs_)
        {
            if (!context_.instances->running(entry.first))
                gone.push_back(entry.first);
        }
        // An instance that is not running is tried afresh when it is.
        std::erase_if(failed_, [this](std::uint64_t number)
                      { return !context_.instances->running(number); });
        for (const std::uint64_t number : gone)
            stop_run(number, true);
        for (const instance_view& view : context_.instances->views())
        {
            if (!context_.may(permission::ports_write) ||
                view.spec.kind != extension_kind::generator ||
                !context_.instances->running(view.spec.instance) ||
                runs_.contains(view.spec.instance))
                continue;
            begin(view.spec);
        }
    }

    generator_counts counts() const override { return counts_; }

private:
    /// A feed's way to the endpoint.  The writes it makes are queued on the
    /// endpoint, and no more than the window waits there at once.
    class endpoint_writer final : public port_writer
    {
    public:
        explicit endpoint_writer(std::shared_ptr<endpoint> target)
            : target_(std::move(target))
        {
        }

        /// Called with the handler that runs when the endpoint is done
        /// with bytes.  Set once.
        std::function<void(std::size_t, bool)> on_done;

        port_write_result write(std::uint64_t, bytes_view data) override
        {
            port_write_result result;
            if (failed)
            {
                result.failed = true;
                return result;
            }
            const std::size_t room =
                outstanding < max_feed_window
                    ? static_cast<std::size_t>(max_feed_window - outstanding)
                    : 0;
            if (room == 0)
            {
                result.not_ready = true;
                return result;
            }
            const std::size_t count = std::min(room, data.size());
            outstanding += count;
            byte_buffer copy(data.begin(),
                             data.begin() + static_cast<std::ptrdiff_t>(count));
            target_->write(std::move(copy),
                           [done = on_done, count](boost::system::error_code ec,
                                                   std::size_t)
                           {
                               if (done)
                               {
                                   done(count,
                                        ec &&
                                            ec != boost::asio::error::
                                                      operation_aborted);
                               }
                           });
            result.taken = count;
            return result;
        }

        /// The name the hub gives the port.
        std::string name() const { return target_->name(); }

        /// Bytes the endpoint was given and has not finished with.
        std::uint64_t outstanding = 0;
        /// A write failed: the port is not usable.
        bool failed = false;

    private:
        std::shared_ptr<endpoint> target_;
    };

    /// One generator: an instance's feed and what it writes to.
    struct generator_run
    {
        generator_run(std::uint64_t instance, std::uint64_t epoch,
                      const feed_options& options,
                      std::shared_ptr<endpoint> target,
                      const boost::asio::any_io_executor& executor)
            : number(instance), generation(epoch), feed(options),
              writer(std::move(target)), timer(executor)
        {
        }

        std::uint64_t number;
        /// Tells this generator from an earlier one of the same instance.
        std::uint64_t generation;
        generator_feed feed;
        endpoint_writer writer;
        boost::asio::steady_timer timer;
        /// `generator.start` was sent and not answered yet.
        bool starting = true;
    };

    void answer(std::uint64_t id, api_message message)
    {
        if (id != 0)
            context_.link.answer(id, std::move(message));
    }

    void refuse(std::uint64_t id, api_errc code, const std::string& text)
    {
        answer(id, error_answer(code, text));
    }

    /// Checks what every generator message needs: the permission and an
    /// instance of kind generator that the plugin holds and is running.
    /// Answers a request that fails, and says whether to go on.
    bool allowed(std::uint64_t id, std::uint64_t instance)
    {
        if (!context_.may(permission::ports_write))
        {
            refuse(id, api_errc::not_granted,
                   "the plugin may not write to ports");
            return false;
        }
        const auto found = context_.instances
                               ? context_.instances->find(instance)
                               : std::nullopt;
        if (!found || !context_.instances->running(instance) ||
            found->spec.kind != extension_kind::generator)
        {
            refuse(id, api_errc::refused,
                   "the instance is not a running generator");
            return false;
        }
        return true;
    }

    generator_run* find_run(std::uint64_t instance)
    {
        const auto found = runs_.find(instance);
        return found == runs_.end() ? nullptr : found->second.get();
    }

    // -- start -----------------------------------------------------------

    void begin(const instance_spec& spec)
    {
        if (failed_.contains(spec.instance))
            return;
        const std::optional<std::string> name = port_named(spec.params);
        std::shared_ptr<endpoint> target;
        if (name && owner_.find_port)
            target = owner_.find_port(*name);
        if (!target)
        {
            // The instance stays as it is: nothing is retried until it
            // is made again.
            ++counts_.start_failed;
            failed_.insert(spec.instance);
            return;
        }
        feed_options options;
        options.port = target->id();
        options.timing = owner_.timing_of ? owner_.timing_of(spec.type)
                                          : feed_timing::async;
        options.window_bytes = max_feed_window;
        options.window_batches = 65536;
        options.max_batch_bytes = max_generator_write;
        const std::uint64_t generation = ++generations_;
        auto made = std::make_unique<generator_run>(spec.instance, generation, options,
                                          std::move(target), context_.executor);
        made->writer.on_done = [alive = alive_, this, number = spec.instance,
                                generation, executor = context_.executor](
                                   std::size_t count, bool failed)
        {
            // Called on the endpoint's strand, possibly after this object
            // has gone: the result goes to the service's, and nothing of
            // this object is touched before the flag is read there.
            boost::asio::post(
                executor,
                [alive, this, number, generation, count, failed]()
                {
                    if (*alive)
                        wrote(number, generation, count, failed);
                });
        };
        generator_run& current = *made;
        runs_.insert_or_assign(spec.instance, std::move(made));

        generator_start_message body;
        body.instance = spec.instance;
        body.place = current.writer.name();
        body.params = as_sent(spec.params);
        auto encoded = encode_as(body, placeholder_id);
        if (!encoded)
        {
            ++counts_.start_failed;
            runs_.erase(spec.instance);
            failed_.insert(spec.instance);
            return;
        }
        context_.link.request(
            std::move(*encoded),
            [alive = alive_, this, number = spec.instance,
             generation](error_code failed, const api_message& reply)
            {
                if (*alive)
                    started(number, generation, failed, reply);
            },
            start_timeout);
    }

    void started(std::uint64_t number, std::uint64_t generation,
                 error_code failed, const api_message& reply)
    {
        generator_run* current = find_run(number);
        if (current == nullptr || current->generation != generation)
            return;
        if (failed || std::holds_alternative<error_message>(reply.body))
        {
            ++counts_.start_failed;
            failed_.insert(number);
            stop_run(number, false);
            return;
        }
        ++counts_.started;
        current->starting = false;
        deal_with(*current, current->feed.start(clock_now()));
    }

    // -- the plugin's messages -------------------------------------------

    void write(const api_message& message)
    {
        const auto asked = decode_as<generator_write_message>(message);
        if (!asked || !context_.may(permission::ports_write) ||
            !context_.instances || !context_.instances->running(asked->instance))
        {
            ++counts_.writes_dropped;
            return;
        }
        sync();
        generator_run* current = find_run(asked->instance);
        if (current == nullptr || current->starting ||
            current->feed.state() == feed_state::ended)
        {
            ++counts_.writes_dropped;
            return;
        }
        feed_batch batch;
        batch.data = asked->data;
        deal_with(*current, current->feed.on_batch(std::move(batch),
                                                   clock_now(),
                                                   current->writer));
    }

    void done(const api_message& message)
    {
        const auto asked = decode_as<generator_done_message>(message);
        if (!asked || !context_.may(permission::ports_write) ||
            !context_.instances || !context_.instances->running(asked->instance))
        {
            ++counts_.writes_dropped;
            return;
        }
        generator_run* current = find_run(asked->instance);
        if (current == nullptr || current->starting)
        {
            ++counts_.writes_dropped;
            return;
        }
        deal_with(*current,
                  current->feed.on_done(clock_now(), current->writer));
    }

    void pattern(const api_message& message)
    {
        const auto asked = decode_as<generator_pattern_message>(message);
        if (!asked)
        {
            refuse(message.id, api_errc::bad_request,
                   "the pattern was not asked for as it must be");
            return;
        }
        if (!allowed(message.id, asked->instance))
            return;
        const auto found = context_.instances->find(asked->instance);
        const feed_timing timing =
            owner_.timing_of && found.has_value()
                ? owner_.timing_of(found->spec.type)
                : feed_timing::async;
        if (timing != feed_timing::rules)
        {
            refuse(message.id, api_errc::not_declared,
                   "the instance did not declare timing class rules");
            return;
        }
        refuse(message.id, api_errc::unsupported,
               "this host cannot make patterns yet");
    }

    // -- what a feed asks for --------------------------------------------

    static feed_time clock_now() { return std::chrono::steady_clock::now(); }

    /// Does what a call to a feed asks of the owner of it.
    void deal_with(generator_run& current, const feed_output& out)
    {
        counts_.bytes_written += out.written;
        const std::uint64_t number = current.number;
        const std::uint64_t generation = current.generation;
        bool ended = false;
        bool misbehaved = false;
        bool told = false;
        for (const feed_event& event : out.events)
        {
            if (std::holds_alternative<feed_refused_event>(event))
            {
                ++counts_.writes_refused;
            }
            else if (const auto* over = std::get_if<feed_ended_event>(&event))
            {
                ended = true;
                misbehaved = over->reason == feed_end_reason::misbehaving;
                told = over->reason != feed_end_reason::done;
            }
        }
        if (out.credit.has_value() && out.credit->bytes > 0 && !ended)
        {
            generator_credit_message credit;
            credit.instance = number;
            credit.bytes = std::min(out.credit->bytes, max_generator_credit);
            auto encoded = encode_as(credit);
            if (encoded)
                context_.link.notify(std::move(*encoded));
        }
        if (ended)
        {
            current.timer.cancel();
            if (misbehaved)
                ++counts_.misbehaved;
            // What an ended generator holds is dropped, and the plugin is
            // told to stop sending.
            if (told)
                send_stop(number);
            return;
        }
        if (out.wake.has_value())
        {
            current.timer.expires_at(*out.wake);
            current.timer.async_wait(
                [alive = alive_, this, number,
                 generation](const boost::system::error_code& ec)
                {
                    if (ec || !*alive)
                        return;
                    woke(number, generation);
                });
        }
        else
        {
            current.timer.cancel();
        }
    }

    void woke(std::uint64_t number, std::uint64_t generation)
    {
        generator_run* current = find_run(number);
        if (current == nullptr || current->generation != generation)
            return;
        deal_with(*current,
                  current->feed.on_time(clock_now(), current->writer));
    }

    /// An endpoint finished with `count` bytes of a generator.
    void wrote(std::uint64_t number, std::uint64_t generation,
               std::size_t count, bool failed)
    {
        generator_run* current = find_run(number);
        if (current == nullptr || current->generation != generation)
            return;
        current->writer.outstanding -=
            std::min<std::uint64_t>(current->writer.outstanding, count);
        if (failed)
        {
            ++counts_.write_errors;
            current->writer.failed = true;
        }
        if (current->feed.state() == feed_state::ended)
            return;
        deal_with(*current,
                  current->feed.on_port_ready(clock_now(), current->writer));
    }

    // -- the end -----------------------------------------------------------

    /// Tells the plugin to stop the generator of an instance.  The answer
    /// is of no use: nothing more is done for the instance either way.
    void send_stop(std::uint64_t number)
    {
        generator_stop_message body;
        body.instance = number;
        auto encoded = encode_as(body, placeholder_id);
        if (!encoded || !context_.link.request)
            return;
        context_.link.request(
            std::move(*encoded), [](error_code, const api_message&) {},
            stop_timeout);
    }

    /// Stops one generator.  `tell`: the plugin is still there and is told.
    void stop_run(std::uint64_t number, bool tell)
    {
        const auto found = runs_.find(number);
        if (found == runs_.end())
            return;
        generator_run& current = *found->second;
        current.timer.cancel();
        const bool was_over = current.feed.state() == feed_state::ended;
        current.feed.stop();
        if (tell && !was_over)
            send_stop(number);
        runs_.erase(found);
    }

    void stop_all()
    {
        for (auto& [number, current] : runs_)
        {
            current->timer.cancel();
            current->feed.stop();
        }
        runs_.clear();
        failed_.clear();
    }

    extension_context context_;
    generator_owner owner_;
    /// False once this object has gone, for what was posted and runs
    /// later.  Read and written on the service's strand only.
    std::shared_ptr<bool> alive_;
    std::map<std::uint64_t, std::unique_ptr<generator_run>> runs_;
    /// Instances that could not be started: not tried again until the
    /// plugin starts again.
    std::set<std::uint64_t> failed_;
    std::uint64_t generations_ = 0;
    generator_counts counts_;
};

} // namespace

std::unique_ptr<generator_point>
make_generator_point(const extension_context& context, generator_owner owner)
{
    return std::make_unique<generator_handler>(context, std::move(owner));
}

} // namespace vss::plugin::host
