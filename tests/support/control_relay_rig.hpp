//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A control relay on a strand of its own, for the tests of
// `vss::plugin::host::control_relay`.  The rig runs one thread; every call to
// the relay is posted to its strand, and what the relay's handlers say is
// kept so that a test can wait for it with a limit and then read it.  The
// owner's answer to an approval can be given at once, refused, or held until
// the test gives it.
//

#pragma once

#include <vss/plugin/host/control_relay.hpp>

#include "test_framework.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vss_test_relay
{

/// The longest any wait lasts.
inline constexpr std::chrono::milliseconds long_limit{20000};

/// One frame the relay offered to the plugin.
struct offered_frame
{
    std::uint64_t session = 0;
    std::string header{};
    vss::byte_buffer blob{};
    /// Whether the handler took it.
    bool taken = true;
};

/// One `closed` the relay called.
struct closed_note
{
    std::uint64_t session = 0;
    vss::plugin::host::control_close_code code =
        vss::plugin::host::control_close_code::gateway_closed;
    std::string reason{};
};

/// What the owner does when asked.
enum class approval_mode
{
    approve,
    deny,
    /// The answer is kept until `release`.
    hold
};

class relay_rig
{
public:
    explicit relay_rig(vss::plugin::host::control_relay_options options)
        : guard_(boost::asio::make_work_guard(io_)),
          strand_(boost::asio::make_strand(io_))
    {
        vss::plugin::host::control_relay_handlers handlers;
        handlers.approve =
            [this](const vss::plugin::host::control_approval_request& ask,
                   std::function<void(bool)> answer)
        { asked(ask, std::move(answer)); };
        handlers.to_plugin = [this](std::uint64_t session,
                                    std::string_view header,
                                    vss::bytes_view blob)
        { return offered(session, header, blob); };
        handlers.closed = [this](std::uint64_t session,
                                 vss::plugin::host::control_close_code code,
                                 std::string_view reason)
        {
            {
                const std::lock_guard lock(mutex_);
                closed_.push_back({session, code, std::string(reason)});
            }
            changed_.notify_all();
        };
        handlers.log = [this](std::string_view line)
        {
            {
                const std::lock_guard lock(mutex_);
                lines_.emplace_back(line);
            }
            changed_.notify_all();
        };
        relay_ = vss::plugin::host::control_relay::create(
            strand_, std::move(options), std::move(handlers));
        thread_ = std::thread([this] { io_.run(); });
    }

    /// Closes what is open, lets what the relay still has to do finish and
    /// stops the thread.
    ~relay_rig()
    {
        std::promise<void> destroyed;
        std::future<void> gone = destroyed.get_future();
        boost::asio::post(strand_,
                          [this, &destroyed]
                          {
                              relay_.reset();
                              destroyed.set_value();
                          });
        gone.wait();
        guard_.reset();
        thread_.join();
    }

    relay_rig(const relay_rig&) = delete;
    relay_rig& operator=(const relay_rig&) = delete;

    // -- the owner ------------------------------------------------------------

    void set_mode(approval_mode mode)
    {
        const std::lock_guard lock(mutex_);
        mode_ = mode;
    }

    /// Answers the oldest held approval.  Callable from the test thread: the
    /// relay takes the answer from any thread.
    void release(bool approved)
    {
        std::function<void(bool)> answer;
        {
            const std::lock_guard lock(mutex_);
            REQUIRE(!held_.empty());
            answer = std::move(held_.front());
            held_.erase(held_.begin());
        }
        answer(approved);
    }

    std::vector<vss::plugin::host::control_approval_request> approvals() const
    {
        const std::lock_guard lock(mutex_);
        return approvals_;
    }

    // -- the plugin -----------------------------------------------------------

    /// Starts `request_session` and gives the answer when it comes.
    std::shared_future<vss::plugin::host::control_session_answer>
    request_async(vss::plugin::host::control_session_request request)
    {
        auto answer = std::make_shared<
            std::promise<vss::plugin::host::control_session_answer>>();
        std::shared_future<vss::plugin::host::control_session_answer> future =
            answer->get_future().share();
        boost::asio::post(
            strand_,
            [this, answer, request = std::move(request)]() mutable
            {
                relay_->request_session(
                    std::move(request),
                    [this,
                     answer](vss::plugin::host::control_session_answer given)
                    {
                        note_answer(given);
                        answer->set_value(std::move(given));
                    });
            });
        return future;
    }

    /// Makes a request and waits for the answer.
    vss::plugin::host::control_session_answer
    request(vss::plugin::host::control_session_request request)
    {
        const auto future = request_async(std::move(request));
        if (future.wait_for(long_limit) != std::future_status::ready)
            throw vss_test::failure("the relay did not answer a request");
        return future.get();
    }

    /// Opens a session; fails the test when it is refused.
    vss::plugin::host::control_session_opened
    open(std::vector<std::string> scopes, std::string peer = "a remote host")
    {
        vss::plugin::host::control_session_request asking;
        asking.scopes = std::move(scopes);
        asking.peer = std::move(peer);
        asking.reason = "a test";
        const auto answer = request(std::move(asking));
        if (const auto* opened =
                std::get_if<vss::plugin::host::control_session_opened>(&answer))
            return *opened;
        const auto* refused =
            std::get_if<vss::plugin::host::control_session_refused>(&answer);
        throw vss_test::failure(
            "the session was refused: " +
            (refused != nullptr ? refused->text : std::string("?")));
    }

    void send(std::uint64_t session, std::string header,
              vss::byte_buffer blob = {})
    {
        boost::asio::post(strand_,
                          [this, session, header = std::move(header),
                           blob = std::move(blob)]() mutable
                          {
                              relay_->frame_from_plugin(
                                  session, std::move(header), std::move(blob));
                          });
    }

    void has_room(std::uint64_t session)
    {
        boost::asio::post(strand_, [this, session]
                          { relay_->plugin_has_room(session); });
    }

    void close_session(std::uint64_t session)
    {
        boost::asio::post(strand_,
                          [this, session] { relay_->close_session(session); });
    }

    /// Closes everything and waits until the relay has revoked the tokens.
    void close_all()
    {
        auto finished = std::make_shared<std::promise<void>>();
        std::future<void> done = finished->get_future();
        boost::asio::post(
            strand_, [this, finished]
            { relay_->close_all([finished] { finished->set_value(); }); });
        if (done.wait_for(long_limit) != std::future_status::ready)
            throw vss_test::failure("close_all did not finish");
    }

    /// Runs `work` on the relay's strand and waits for it.
    void on_strand(const std::function<void()>& work)
    {
        std::promise<void> finished;
        std::future<void> done = finished.get_future();
        boost::asio::post(strand_,
                          [&work, &finished]
                          {
                              work();
                              finished.set_value();
                          });
        done.wait();
    }

    std::size_t session_count()
    {
        std::size_t count = 0;
        on_strand([this, &count] { count = relay_->session_count(); });
        return count;
    }

    // -- what the relay did ---------------------------------------------------

    /// How many of the next offers the `to_plugin` handler refuses.
    void refuse_offers(std::size_t count)
    {
        const std::lock_guard lock(mutex_);
        refusals_ = count;
    }

    /// Waits until `condition` holds for what the rig has recorded.  The
    /// condition runs with the rig's lock held.
    template <class Condition>
    bool wait_until(Condition condition,
                    std::chrono::milliseconds limit = long_limit)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, limit, [&] { return condition(); });
    }

    /// Waits until the owner has been asked `count` times.
    bool wait_approvals(std::size_t count)
    {
        return wait_until([&] { return approvals_.size() >= count; });
    }

    /// Waits for a frame the relay offered that `match` accepts, and gives it.
    /// `match` runs with the rig's lock held.
    template <class Predicate>
    std::optional<offered_frame>
    wait_frame(Predicate match, std::chrono::milliseconds limit = long_limit)
    {
        std::optional<offered_frame> found;
        wait_until(
            [&]
            {
                for (const offered_frame& item : offered_)
                {
                    if (match(item))
                    {
                        found = item;
                        return true;
                    }
                }
                return false;
            },
            limit);
        return found;
    }

    std::vector<offered_frame> frames() const
    {
        const std::lock_guard lock(mutex_);
        return offered_;
    }

    std::vector<closed_note> closed() const
    {
        const std::lock_guard lock(mutex_);
        return closed_;
    }

    std::vector<std::string> lines() const
    {
        const std::lock_guard lock(mutex_);
        return lines_;
    }

    /// Waits for the `closed` of a session and gives its reason.
    std::string wait_closed(std::uint64_t session)
    {
        std::string reason;
        const bool seen = wait_until(
            [&]
            {
                for (const closed_note& note : closed_)
                {
                    if (note.session == session)
                    {
                        reason = note.reason;
                        return true;
                    }
                }
                return false;
            });
        if (!seen)
            throw vss_test::failure("the session was not closed");
        return reason;
    }

    /// Waits for the `closed` of a session that is number `index` for it
    /// (0 for the first), and gives its code.
    vss::plugin::host::control_close_code wait_code(std::uint64_t session,
                                                    std::size_t index = 0)
    {
        vss::plugin::host::control_close_code code =
            vss::plugin::host::control_close_code::gateway_closed;
        const bool seen = wait_until(
            [&]
            {
                std::size_t count = 0;
                for (const closed_note& note : closed_)
                {
                    if (note.session != session)
                        continue;
                    if (count++ == index)
                    {
                        code = note.code;
                        return true;
                    }
                }
                return false;
            });
        if (!seen)
            throw vss_test::failure("the session was not closed");
        return code;
    }

    /// Waits until the relay has called `closed` `count` times for a session.
    bool wait_closed_count(std::uint64_t session, std::size_t count)
    {
        return wait_until(
            [&]
            {
                std::size_t seen = 0;
                for (const closed_note& note : closed_)
                    seen += note.session == session ? 1 : 0;
                return seen >= count;
            });
    }

    /// Every text the relay gave out: approvals, answers, reasons, log lines
    /// and the headers it passed to the plugin.  For a search for a secret.
    std::string everything_said() const
    {
        const std::lock_guard lock(mutex_);
        std::string text;
        const auto add = [&text](const std::string& line)
        {
            text.append(line);
            text.push_back('\n');
        };
        for (const auto& ask : approvals_)
        {
            add(ask.plugin_id);
            add(ask.plugin_name);
            add(ask.peer);
            add(ask.reason);
            for (const std::string& scope : ask.scopes)
                add(scope);
        }
        for (const std::string& line : answer_texts_)
            add(line);
        for (const closed_note& note : closed_)
            add(note.reason);
        for (const std::string& line : lines_)
            add(line);
        for (const offered_frame& item : offered_)
            add(item.header);
        return text;
    }

private:
    void asked(const vss::plugin::host::control_approval_request& ask,
               std::function<void(bool)> answer)
    {
        approval_mode mode = approval_mode::approve;
        {
            const std::lock_guard lock(mutex_);
            approvals_.push_back(ask);
            mode = mode_;
            if (mode == approval_mode::hold)
                held_.push_back(answer);
        }
        changed_.notify_all();
        if (mode == approval_mode::approve)
            answer(true);
        else if (mode == approval_mode::deny)
            answer(false);
    }

    bool offered(std::uint64_t session, std::string_view header,
                 vss::bytes_view blob)
    {
        bool taken = true;
        {
            const std::lock_guard lock(mutex_);
            if (refusals_ > 0)
            {
                --refusals_;
                taken = false;
            }
            offered_.push_back({session, std::string(header),
                                vss::byte_buffer(blob.begin(), blob.end()),
                                taken});
        }
        changed_.notify_all();
        return taken;
    }

    void note_answer(const vss::plugin::host::control_session_answer& given)
    {
        {
            const std::lock_guard lock(mutex_);
            if (const auto* refused =
                    std::get_if<vss::plugin::host::control_session_refused>(
                        &given))
                answer_texts_.push_back(refused->text);
            else if (const auto* opened =
                         std::get_if<vss::plugin::host::control_session_opened>(
                             &given))
                for (const std::string& scope : opened->scopes)
                    answer_texts_.push_back(scope);
        }
        changed_.notify_all();
    }

    boost::asio::io_context io_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type>
        guard_;
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    std::shared_ptr<vss::plugin::host::control_relay> relay_;
    std::thread thread_;

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    approval_mode mode_ = approval_mode::approve;
    std::vector<std::function<void(bool)>> held_;
    std::vector<vss::plugin::host::control_approval_request> approvals_;
    std::vector<offered_frame> offered_;
    std::vector<closed_note> closed_;
    std::vector<std::string> lines_;
    std::vector<std::string> answer_texts_;
    std::size_t refusals_ = 0;
};

} // namespace vss_test_relay
