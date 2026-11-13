//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "test_framework.hpp"

#include "qt_input.hpp"
#include "qt_wait.hpp"

#include "backend/engine_studio_backend.hpp"
#include "backend/fake_studio_backend.hpp"
#include "simple/job_form.hpp"
#include "simple/job_grid.hpp"
#include "simple/job_plan.hpp"
#include "simple/setup_kinds.hpp"
#include "simple/setup_page.hpp"
#include "simple/simple_page.hpp"
#include "theme/theme.hpp"
#include "widgets/notes.hpp"

#include <vss/core/link.hpp>
#include <vss/studio/studio.hpp>

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace
{

using namespace vss::gui;
using namespace vss::gui::simple;

void with_devices(FakeStudioBackend& backend)
{
    device_view com3;
    com3.path = QStringLiteral("COM3");
    com3.description = QStringLiteral("USB Serial Device");
    device_view com4;
    com4.path = QStringLiteral("COM4");
    com4.description = QStringLiteral("Prolific USB-to-Serial");
    backend.set_devices({com3, com4});
}

/// Submits a job form and waits for its worker thread.  True when the setup
/// was made, which is when the form says `done`.
bool submit_and_wait(JobForm* form)
{
    const QPointer<JobForm> guard(form);
    bool made = false;
    // The connection ends with this call: `made` is a local.
    const QMetaObject::Connection connection = QObject::connect(
        form, &JobForm::done,
        [&made](vss::endpoint_id, const setup_prefs&) { made = true; });
    if (!form->submit())
    {
        QObject::disconnect(connection);
        return false;
    }
    const bool finished = vss_test::wait_until(
        [&guard] { return guard.isNull() || !guard->busy(); });
    QObject::disconnect(connection);
    REQUIRE(finished);
    return made;
}

capabilities windows_caps()
{
    capabilities caps;
    caps.com_numbers = true;
    caps.fixture_echo = true;
    caps.shared_bus = true;
    caps.rename_port = true;
    return caps;
}

std::vector<setup_view> setups(const FakeStudioBackend& backend)
{
    return list_setups(backend.ports(), backend.links());
}

job_plan share_plan(int programs)
{
    job_plan plan;
    plan.kind = job::share;
    end_plan device;
    device.what = end_plan::kind::device;
    device.device_path = QStringLiteral("COM3");
    plan.first.push_back(device);
    for (int index = 0; index < programs; ++index)
    {
        end_plan program;
        program.label = index == 0 ? QStringLiteral("PuTTY") : QString();
        program.com_number = static_cast<unsigned>(5 + index);
        plan.second.push_back(program);
    }
    return plan;
}

QStringList visible_texts(QWidget* root)
{
    QStringList texts;
    for (auto* label : root->findChildren<QLabel*>())
        texts << label->text();
    for (auto* button : root->findChildren<QAbstractButton*>())
        texts << button->text();
    return texts;
}

void settle()
{
    for (int round = 0; round < 5; ++round)
    {
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents();
    }
}

} // namespace

TEST_CASE("simple: setup kinds are read back from ports and links")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());

    const auto lone = backend.create_auto_port(QStringLiteral("program"), true);
    auto found = setups(backend);
    REQUIRE_EQ(found.size(), std::size_t{1});
    REQUIRE(found[0].kind == job::port);

    const auto a = backend.create_auto_port(QStringLiteral("a"), true);
    const auto b = backend.create_auto_port(QStringLiteral("b"), true);
    REQUIRE(backend.connect_ports(*a, *b, {}).has_value());
    const auto device = backend.attach_serial(
        QStringLiteral("device-com3"),
        QStringLiteral("serial://COM3?baud=9600"), {}, true);
    const auto c = backend.create_auto_port(QStringLiteral("c"), true);
    const auto d = backend.create_auto_port(QStringLiteral("d"), true);
    REQUIRE(backend.split(*device, {*c, *d}, {}).has_value());
    const auto talk = backend.attach_serial(
        QStringLiteral("device-com4"),
        QStringLiteral("serial://COM4?baud=9600"), {}, true);
    const auto e = backend.create_auto_port(QStringLiteral("e"), true);
    const auto f = backend.create_auto_port(QStringLiteral("f"), true);
    REQUIRE(backend.redirect(*e, *f, {}).has_value());

    found = setups(backend);
    const auto kind_of = [&found](vss::endpoint_id id)
    {
        for (const auto& setup : found)
        {
            if (setup.group.ports.contains(id))
                return setup.kind;
        }
        return job::custom;
    };
    REQUIRE(kind_of(*lone) == job::port);
    REQUIRE(kind_of(*a) == job::pair);
    REQUIRE(kind_of(*device) == job::share);
    REQUIRE(kind_of(*talk) == job::talk);
    // A one-way redirect is no Simple-mode job, so it is shown as made in
    // Advanced mode.
    REQUIRE(kind_of(*e) == job::custom);

    for (const auto& setup : found)
    {
        if (setup.kind == job::share)
        {
            REQUIRE_EQ(setup_title(setup, backend.ports()),
                       QStringLiteral("COM3 shared with 2 programs"));
            REQUIRE_EQ(setup.programs.size(), qsizetype{2});
        }
    }
}

TEST_CASE("simple: build a share, add and close a program without touching the "
          "others")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    with_devices(backend);

    const auto built = build_setup(backend, share_plan(2));
    REQUIRE(built.has_value());
    auto found = setups(backend);
    REQUIRE_EQ(found.size(), std::size_t{1});
    REQUIRE(found[0].kind == job::share);
    const auto first_program = found[0].programs[0];
    const auto second_program = found[0].programs[1];
    REQUIRE_EQ(vss_test::require_value(backend.port(first_program)).name,
               QStringLiteral("PuTTY"));
    REQUIRE_EQ(vss_test::require_value(backend.port(first_program)).path,
               QStringLiteral("COM5"));
    REQUIRE_EQ(
        program_label(vss_test::require_value(backend.port(second_program))),
        QString());

    // Add one: the two ports that were there keep their identities, and the
    // link stays the same link.
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    const vss::link_id link_id = backend.links().front().id;
    job_plan plan = plan_from(found[0], backend.ports());
    plan.second.push_back(end_plan{});
    REQUIRE(build_setup(backend, plan, &found[0]).has_value());
    found = setups(backend);
    REQUIRE_EQ(found.size(), std::size_t{1});
    REQUIRE_EQ(found[0].programs.size(), qsizetype{3});
    REQUIRE(found[0].programs.contains(first_program));
    REQUIRE(found[0].programs.contains(second_program));
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    REQUIRE_EQ(backend.links().front().id, link_id);
    REQUIRE_EQ(found[0].link, link_id);

    // Close the second: it goes, the others stay.
    plan = plan_from(found[0], backend.ports());
    plan.second.erase(
        std::remove_if(plan.second.begin(), plan.second.end(),
                       [second_program](const end_plan& end)
                       { return end.existing == second_program; }),
        plan.second.end());
    const plan_changes changes = compare(plan, found[0]);
    REQUIRE_EQ(changes.closed.size(), std::size_t{1});
    REQUIRE_EQ(changes.closed.front(), second_program);
    REQUIRE(build_setup(backend, plan, &found[0]).has_value());
    REQUIRE(!backend.port(second_program).has_value());
    found = setups(backend);
    REQUIRE_EQ(found[0].programs.size(), qsizetype{2});
    REQUIRE(found[0].programs.contains(first_program));
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    REQUIRE_EQ(backend.links().front().id, link_id);

    // Close down to one program: still the same link, now an attached device.
    plan = plan_from(found[0], backend.ports());
    plan.kind = job::attach;
    plan.second.erase(std::remove_if(plan.second.begin(), plan.second.end(),
                                     [first_program](const end_plan& end)
                                     { return end.existing == first_program; }),
                      plan.second.end());
    REQUIRE(build_setup(backend, plan, &found[0]).has_value());
    found = setups(backend);
    REQUIRE_EQ(found.size(), std::size_t{1});
    REQUIRE(found[0].kind == job::attach);
    REQUIRE_EQ(backend.links().front().id, link_id);
}

TEST_CASE("simple: a failed build leaves nothing behind")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    job_plan plan;
    plan.kind = job::combine;
    plan.first = {end_plan{}, end_plan{}};
    end_plan gone;
    gone.existing = 9999; // a receiver that does not exist
    plan.second = {gone};
    const auto built = build_setup(backend, plan);
    REQUIRE(!built.has_value());
    REQUIRE(backend.ports().empty());
}

namespace
{

job_plan turns_plan(int programs)
{
    job_plan plan;
    plan.kind = job::turns;
    for (int index = 0; index < programs; ++index)
        plan.first.push_back(end_plan{});
    end_plan device;
    device.what = end_plan::kind::device;
    device.device_path = QStringLiteral("COM4");
    plan.second = {device};
    return plan;
}

void close_program(job_plan& plan, vss::endpoint_id id)
{
    auto& programs = plan.kind == job::share || plan.kind == job::attach
                         ? plan.second
                         : plan.first;
    programs.erase(std::remove_if(programs.begin(), programs.end(),
                                  [id](const end_plan& end)
                                  { return end.existing == id; }),
                   programs.end());
}

} // namespace

TEST_CASE("simple: take turns is a split with the owner policy and the first "
          "program sends")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    with_devices(backend);
    REQUIRE(build_setup(backend, turns_plan(2)).has_value());
    const auto found = setups(backend);
    REQUIRE_EQ(found.size(), std::size_t{1});
    REQUIRE(found[0].kind == job::turns);
    REQUIRE_EQ(found[0].owner, found[0].programs[0]);

    // One duplex link from the device to the programs, so the device's
    // replies reach every program; only the owner's bytes go to the device.
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    const link_view link = backend.links().front();
    REQUIRE(link.mode == vss::link_mode::duplex);
    REQUIRE(link.shape == link_shape::split);
    REQUIRE(link.options.policy == vss::merge_policy::owner);
    REQUIRE_EQ(link.sources.size(), qsizetype{1});
    REQUIRE_EQ(link.sources.front(), found[0].device);
    REQUIRE_EQ(link.targets.size(), qsizetype{2});
    REQUIRE_EQ(link.owner, found[0].programs[0]);
    REQUIRE_EQ(setup_title(found[0], backend.ports()),
               QStringLiteral("2 programs take turns sending to COM4"));
}

namespace
{

port_view hand_port(vss::endpoint_id id, vss::endpoint_kind kind,
                    const QString& name)
{
    port_view port;
    port.id = id;
    port.kind = kind;
    port.name = name;
    port.path = name;
    return port;
}

/// A link as the real engine adapter reports it: a duplex link with one source
/// and one target is a two-way link, whatever its policy.
link_view hand_link(vss::link_id id, vss::link_mode mode,
                    vss::endpoint_id source, vss::endpoint_id target)
{
    link_view link;
    link.id = id;
    link.shape = link_shape::two_way;
    link.mode = mode;
    link.sources = {source};
    link.targets = {target};
    return link;
}

template <typename... Ids> QVector<vss::endpoint_id> ids(Ids... id)
{
    return QVector<vss::endpoint_id>{static_cast<vss::endpoint_id>(id)...};
}

setup_view only_setup(const std::vector<port_view>& ports,
                      const std::vector<link_view>& links)
{
    const auto found = list_setups(ports, links);
    if (found.size() != 1)
        throw std::runtime_error("expected one setup");
    return found.front();
}

} // namespace

TEST_CASE("simple: a device and one program on a two-way link is an attach")
{
    const std::vector<port_view> ports = {
        hand_port(1, vss::endpoint_kind::physical, QStringLiteral("COM3")),
        hand_port(2, vss::endpoint_kind::virtual_port, QStringLiteral("COM5")),
    };

    // No policy set: the device is attached to the program.
    auto link = hand_link(10, vss::link_mode::duplex, 1, 2);
    link.options.policy = vss::merge_policy::interleave;
    auto setup = only_setup(ports, {link});
    REQUIRE(setup.kind == job::attach);
    REQUIRE_EQ(setup.device, vss::endpoint_id{1});
    REQUIRE(setup.programs == ids(2));
    REQUIRE_EQ(setup.link, vss::link_id{10});
    REQUIRE_EQ(setup.owner, vss::endpoint_id{0});
    REQUIRE_EQ(setup_title(setup, ports), QStringLiteral("COM3 through COM5"));

    // The owner policy is a take-turns setup closed down to one program, and
    // it keeps who sends.
    link.options.policy = vss::merge_policy::owner;
    link.owner = 2;
    setup = only_setup(ports, {link});
    REQUIRE(setup.kind == job::turns);
    REQUIRE_EQ(setup.device, vss::endpoint_id{1});
    REQUIRE(setup.programs == ids(2));
    REQUIRE_EQ(setup.owner, vss::endpoint_id{2});
    link.owner = 0;
    REQUIRE_EQ(only_setup(ports, {link}).owner, vss::endpoint_id{0});
}

TEST_CASE("simple: a network device and one program on a two-way link is an "
          "attach")
{
    for (const auto kind : {vss::endpoint_kind::tcp, vss::endpoint_kind::udp,
                            vss::endpoint_kind::rfc2217})
    {
        const std::vector<port_view> ports = {
            hand_port(1, kind, QStringLiteral("remote")),
            hand_port(2, vss::endpoint_kind::virtual_port,
                      QStringLiteral("COM5")),
        };
        const auto setup =
            only_setup(ports, {hand_link(10, vss::link_mode::duplex, 1, 2)});
        REQUIRE(setup.kind == job::attach);
        REQUIRE_EQ(setup.device, vss::endpoint_id{1});
    }
}

TEST_CASE("simple: two-way links that are not a device and a program keep "
          "their meaning")
{
    const auto device =
        hand_port(1, vss::endpoint_kind::physical, QStringLiteral("COM3"));
    const auto program =
        hand_port(2, vss::endpoint_kind::virtual_port, QStringLiteral("COM5"));
    const auto other_program =
        hand_port(3, vss::endpoint_kind::virtual_port, QStringLiteral("COM6"));
    const auto network =
        hand_port(4, vss::endpoint_kind::tcp, QStringLiteral("tcp://:5000"));
    const auto mock =
        hand_port(5, vss::endpoint_kind::mock, QStringLiteral("echo"));

    // The program is the source and the device the target: not how a setup is
    // made, so it stays as made in Advanced mode.
    auto setup = only_setup({device, program},
                            {hand_link(10, vss::link_mode::duplex, 2, 1)});
    REQUIRE(setup.kind == job::custom);
    REQUIRE(setup.programs.isEmpty());
    REQUIRE_EQ(setup.device, vss::endpoint_id{0});

    // A one-way link from a device to a program is not an attach either.
    setup = only_setup({device, program},
                       {hand_link(10, vss::link_mode::one_way, 1, 2)});
    REQUIRE(setup.kind == job::custom);

    // A device on this PC and a network port is a bridge, either way round.
    setup = only_setup({device, network},
                       {hand_link(10, vss::link_mode::duplex, 1, 4)});
    REQUIRE(setup.kind == job::network);
    REQUIRE_EQ(setup.device, vss::endpoint_id{1});
    REQUIRE_EQ(setup.listener, vss::endpoint_id{4});
    setup = only_setup({device, network},
                       {hand_link(10, vss::link_mode::duplex, 4, 1)});
    REQUIRE(setup.kind == job::network);
    REQUIRE_EQ(setup.device, vss::endpoint_id{1});
    REQUIRE_EQ(setup.listener, vss::endpoint_id{4});

    // A test device answering a program is a port with an echo.
    for (const bool mock_first : {true, false})
    {
        setup = only_setup({program, mock},
                           {mock_first
                                ? hand_link(10, vss::link_mode::duplex, 5, 2)
                                : hand_link(10, vss::link_mode::duplex, 2, 5)});
        REQUIRE(setup.kind == job::port);
        REQUIRE(setup.programs == ids(2));
        REQUIRE_EQ(setup.echo, vss::endpoint_id{5});
        REQUIRE_EQ(setup.device, vss::endpoint_id{0});
    }

    // Two programs are a pair.
    setup = only_setup({program, other_program},
                       {hand_link(10, vss::link_mode::duplex, 2, 3)});
    REQUIRE(setup.kind == job::pair);
    REQUIRE(setup.programs == ids(2, 3));
}

TEST_CASE("simple: the real engine's one-program setups are read back")
{
    // A listening network port stands for the device: it is a device kind that
    // needs no hardware, and it only binds the loopback address.
    vss::studio studio;
    EngineStudioBackend backend(studio);
    const auto device = backend.attach_serial(
        QStringLiteral("device"), QStringLiteral("tcp-listen://127.0.0.1:0"),
        {}, true);
    REQUIRE(device.has_value());
    const auto first = backend.create_virtual_port(
        QStringLiteral("first"), vss::virtual_port_kind::loopback, true);
    const auto second = backend.create_virtual_port(
        QStringLiteral("second"), vss::virtual_port_kind::loopback, true);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(vss_test::require_value(backend.port(*device)).kind ==
            vss::endpoint_kind::tcp);

    // One program: the adapter reports a two-way link, which Simple mode reads
    // as the device attached to the program.
    const auto link = backend.split(*device, {*first}, {});
    REQUIRE(link.has_value());
    REQUIRE(vss_test::require_value(backend.link(*link)).shape ==
            link_shape::two_way);
    const auto setup_of = [&backend](vss::endpoint_id device_id)
    {
        const auto found = list_setups(backend.ports(), backend.links());
        const auto it = std::find_if(found.begin(), found.end(),
                                     [device_id](const setup_view& setup)
                                     { return setup.device == device_id; });
        if (it == found.end())
            throw std::runtime_error("no setup for the device");
        return *it;
    };
    auto setup = setup_of(*device);
    REQUIRE(setup.kind == job::attach);
    REQUIRE(setup.programs == ids(*first));

    // A second program makes it a share.
    REQUIRE(backend.add_target(*link, *second).has_value());
    setup = setup_of(*device);
    REQUIRE(setup.kind == job::share);
    REQUIRE_EQ(setup.programs.size(), qsizetype{2});

    // Taking turns, closed down to one program, is still taking turns.
    vss::link_options turns;
    turns.policy = vss::merge_policy::owner;
    const auto other = backend.create_virtual_port(
        QStringLiteral("other"), vss::virtual_port_kind::loopback, true);
    const auto third = backend.create_virtual_port(
        QStringLiteral("third"), vss::virtual_port_kind::loopback, true);
    REQUIRE(other.has_value());
    REQUIRE(third.has_value());
    const auto next = backend.attach_serial(
        QStringLiteral("device-2"), QStringLiteral("tcp-listen://127.0.0.1:0"),
        {}, true);
    REQUIRE(next.has_value());
    const auto turn_link = backend.split(*next, {*other, *third}, turns);
    REQUIRE(turn_link.has_value());
    REQUIRE(backend.set_owner(*turn_link, *third).has_value());
    REQUIRE(backend.remove_target(*turn_link, *other).has_value());
    REQUIRE(vss_test::require_value(backend.link(*turn_link)).shape ==
            link_shape::two_way);
    setup = setup_of(*next);
    REQUIRE(setup.kind == job::turns);
    REQUIRE(setup.programs == ids(*third));
    REQUIRE_EQ(setup.owner, *third);
}

TEST_CASE("simple: a join with an owner is shown as made in Advanced mode")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    const auto device = backend.attach_serial(
        QStringLiteral("device-com4"),
        QStringLiteral("serial://COM4?baud=9600"), {}, true);
    const auto a = backend.create_auto_port(QStringLiteral("a"), true);
    const auto b = backend.create_auto_port(QStringLiteral("b"), true);
    REQUIRE(device.has_value());
    vss::link_options options;
    options.policy = vss::merge_policy::owner;
    const auto joined = backend.join({*a, *b}, *device, options);
    REQUIRE(joined.has_value());
    REQUIRE(backend.set_owner(*joined, *a).has_value());

    const auto found = setups(backend);
    REQUIRE_EQ(found.size(), std::size_t{1});
    REQUIRE(found[0].kind == job::custom);
    REQUIRE_EQ(found[0].owner, vss::endpoint_id{0});
    REQUIRE(found[0].programs.isEmpty());
}

TEST_CASE("simple: changing take turns keeps the link and the owner")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    with_devices(backend);
    REQUIRE(build_setup(backend, turns_plan(2)).has_value());
    auto found = setups(backend);
    REQUIRE_EQ(found.size(), std::size_t{1});
    const vss::link_id link_id = backend.links().front().id;
    const vss::endpoint_id first = found[0].programs[0];
    const vss::endpoint_id second = found[0].programs[1];

    // The second program has the turn.
    REQUIRE(backend.set_owner(link_id, second).has_value());
    found = setups(backend);
    REQUIRE_EQ(found[0].owner, second);

    // Add a program: same link, same owner, the others stay.
    job_plan plan = plan_from(found[0], backend.ports());
    plan.first.push_back(end_plan{});
    REQUIRE(build_setup(backend, plan, &found[0]).has_value());
    found = setups(backend);
    REQUIRE_EQ(found.size(), std::size_t{1});
    REQUIRE(found[0].kind == job::turns);
    REQUIRE_EQ(found[0].programs.size(), qsizetype{3});
    REQUIRE(found[0].programs.contains(first));
    REQUIRE(found[0].programs.contains(second));
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    REQUIRE_EQ(backend.links().front().id, link_id);
    REQUIRE(backend.links().front().options.policy == vss::merge_policy::owner);
    REQUIRE_EQ(found[0].owner, second);

    // Close one that does not have the turn: same link, same owner.
    plan = plan_from(found[0], backend.ports());
    close_program(plan, first);
    REQUIRE(build_setup(backend, plan, &found[0]).has_value());
    REQUIRE(!backend.port(first).has_value());
    found = setups(backend);
    REQUIRE_EQ(found[0].programs.size(), qsizetype{2});
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    REQUIRE_EQ(backend.links().front().id, link_id);
    REQUIRE_EQ(found[0].owner, second);

    // Close the one that has the turn: same link, the first program left
    // takes the turn.
    plan = plan_from(found[0], backend.ports());
    close_program(plan, second);
    const vss::endpoint_id remaining = found[0].programs[0] == second
                                           ? found[0].programs[1]
                                           : found[0].programs[0];
    REQUIRE(build_setup(backend, plan, &found[0]).has_value());
    REQUIRE(!backend.port(second).has_value());
    found = setups(backend);
    REQUIRE_EQ(found.size(), std::size_t{1});
    REQUIRE_EQ(found[0].programs.size(), qsizetype{1});
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    REQUIRE_EQ(backend.links().front().id, link_id);
    REQUIRE_EQ(backend.links().front().owner, remaining);
    REQUIRE_EQ(found[0].owner, remaining);
}

TEST_CASE("simple: closing the program that has the turn gives the first "
          "program the turn")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    with_devices(backend);
    REQUIRE(build_setup(backend, turns_plan(3)).has_value());
    auto found = setups(backend);
    const vss::link_id link_id = backend.links().front().id;
    const vss::endpoint_id first = found[0].programs[0];
    const vss::endpoint_id second = found[0].programs[1];
    const vss::endpoint_id third = found[0].programs[2];
    REQUIRE_EQ(found[0].owner, first);

    job_plan plan = plan_from(found[0], backend.ports());
    close_program(plan, first);
    REQUIRE(build_setup(backend, plan, &found[0]).has_value());
    found = setups(backend);
    REQUIRE_EQ(backend.links().front().id, link_id);
    REQUIRE_EQ(found[0].programs.size(), qsizetype{2});
    REQUIRE_EQ(found[0].owner, second);
    REQUIRE(found[0].programs.contains(third));
}

TEST_CASE("simple: a failed in-place change leaves the setup as it was")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    with_devices(backend);
    REQUIRE(build_setup(backend, turns_plan(2)).has_value());
    const auto found = setups(backend);
    const vss::link_id link_id = backend.links().front().id;
    const std::size_t ports = backend.ports().size();

    // Adding the program: a port is made, then adding it to the link fails.
    job_plan plan = plan_from(found[0], backend.ports());
    plan.first.push_back(end_plan{});
    backend.fail_after(1, vss::make_error_code(vss::errc::io_error));
    REQUIRE(!build_setup(backend, plan, &found[0]).has_value());
    REQUIRE_EQ(backend.ports().size(), ports);
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    REQUIRE_EQ(backend.links().front().id, link_id);
    REQUIRE_EQ(backend.links().front().targets.size(), qsizetype{2});
    REQUIRE_EQ(backend.links().front().owner, found[0].programs[0]);
}

TEST_CASE("simple: another kind or another policy makes the link again")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    with_devices(backend);
    REQUIRE(build_setup(backend, share_plan(2)).has_value());
    auto found = setups(backend);
    const vss::link_id share_link = backend.links().front().id;

    // A share has the interleave policy; take turns needs the owner policy,
    // so the engine has no in-place edit: the link is replaced.
    job_plan plan = plan_from(found[0], backend.ports());
    plan.kind = job::turns;
    std::swap(plan.first, plan.second);
    REQUIRE(build_setup(backend, plan, &found[0]).has_value());
    found = setups(backend);
    REQUIRE_EQ(found.size(), std::size_t{1});
    REQUIRE(found[0].kind == job::turns);
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    REQUIRE(backend.links().front().id != share_link);
    REQUIRE_EQ(found[0].programs.size(), qsizetype{2});
    REQUIRE_EQ(found[0].owner, found[0].programs[0]);
}

namespace
{

job_plan combine_plan(int senders)
{
    job_plan plan;
    plan.kind = job::combine;
    for (int index = 0; index < senders; ++index)
        plan.first.push_back(end_plan{});
    end_plan receiver;
    receiver.what = end_plan::kind::device;
    receiver.device_path = QStringLiteral("COM4");
    plan.second = {receiver};
    return plan;
}

job_plan bus_plan(int members)
{
    job_plan plan;
    plan.kind = job::bus;
    for (int index = 0; index < members; ++index)
        plan.first.push_back(end_plan{});
    return plan;
}

/// Takes the end that holds `id` out of the plan's first list.
void drop_first(job_plan& plan, vss::endpoint_id id)
{
    plan.first.erase(std::remove_if(plan.first.begin(), plan.first.end(),
                                    [id](const end_plan& end)
                                    { return end.existing == id; }),
                     plan.first.end());
}

} // namespace

TEST_CASE("simple: changing a combine keeps the link and the other senders")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    with_devices(backend);
    REQUIRE(build_setup(backend, combine_plan(2)).has_value());
    auto found = setups(backend);
    REQUIRE_EQ(found.size(), std::size_t{1});
    REQUIRE(found[0].kind == job::combine);
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    const vss::link_id link_id = backend.links().front().id;
    const vss::endpoint_id receiver = found[0].receiver;
    const vss::endpoint_id first = found[0].members[0];
    const vss::endpoint_id second = found[0].members[1];

    // Add a sender: the same link, the senders that were there and the
    // receiver stay.
    job_plan plan = plan_from(found[0], backend.ports());
    plan.first.push_back(end_plan{});
    REQUIRE(build_setup(backend, plan, &found[0]).has_value());
    found = setups(backend);
    REQUIRE_EQ(found.size(), std::size_t{1});
    REQUIRE(found[0].kind == job::combine);
    REQUIRE_EQ(found[0].members.size(), qsizetype{3});
    REQUIRE(found[0].members.contains(first));
    REQUIRE(found[0].members.contains(second));
    REQUIRE_EQ(found[0].receiver, receiver);
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    REQUIRE_EQ(backend.links().front().id, link_id);
    REQUIRE_EQ(found[0].link, link_id);

    // Close the first: it goes, the others stay, and the link is the link.
    plan = plan_from(found[0], backend.ports());
    drop_first(plan, first);
    REQUIRE(build_setup(backend, plan, &found[0]).has_value());
    REQUIRE(!backend.port(first).has_value());
    found = setups(backend);
    REQUIRE_EQ(found.size(), std::size_t{1});
    REQUIRE_EQ(found[0].members.size(), qsizetype{2});
    REQUIRE(found[0].members.contains(second));
    REQUIRE_EQ(found[0].receiver, receiver);
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    REQUIRE_EQ(backend.links().front().id, link_id);
    REQUIRE(backend.port(receiver).has_value());
}

TEST_CASE("simple: a combine with another receiver is made again")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    with_devices(backend);
    REQUIRE(build_setup(backend, combine_plan(2)).has_value());
    auto found = setups(backend);
    const vss::link_id link_id = backend.links().front().id;

    // A different receiver is a different link, so there is no in-place edit.
    job_plan plan = plan_from(found[0], backend.ports());
    end_plan other;
    other.what = end_plan::kind::device;
    other.device_path = QStringLiteral("COM3");
    plan.second = {other};
    REQUIRE(build_setup(backend, plan, &found[0]).has_value());
    found = setups(backend);
    REQUIRE_EQ(found.size(), std::size_t{1});
    REQUIRE(found[0].kind == job::combine);
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    REQUIRE(backend.links().front().id != link_id);
}

TEST_CASE("simple: a failed change of a combine leaves the setup as it was")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    with_devices(backend);
    REQUIRE(build_setup(backend, combine_plan(3)).has_value());
    const auto found = setups(backend);
    const vss::link_id link_id = backend.links().front().id;
    const std::size_t ports = backend.ports().size();
    const auto senders = backend.links().front().sources;
    REQUIRE_EQ(senders.size(), qsizetype{3});

    // A sender is added and another taken off; the second step fails, and the
    // port that was made goes again.
    job_plan plan = plan_from(found[0], backend.ports());
    plan.first.push_back(end_plan{});
    drop_first(plan, senders[0]);
    backend.fail_after(2, vss::make_error_code(vss::errc::io_error));
    REQUIRE(!build_setup(backend, plan, &found[0]).has_value());
    REQUIRE_EQ(backend.ports().size(), ports);
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    REQUIRE_EQ(backend.links().front().id, link_id);
    REQUIRE(backend.links().front().sources == senders);

    // Two senders are taken off and the second step fails: the first comes
    // back, at the end of the list.
    plan = plan_from(found[0], backend.ports());
    drop_first(plan, senders[0]);
    drop_first(plan, senders[1]);
    backend.fail_after(1, vss::make_error_code(vss::errc::io_error));
    REQUIRE(!build_setup(backend, plan, &found[0]).has_value());
    REQUIRE_EQ(backend.ports().size(), ports);
    REQUIRE_EQ(backend.links().front().id, link_id);
    REQUIRE_EQ(backend.links().front().sources.size(), qsizetype{3});
    for (const auto id : senders)
        REQUIRE(backend.links().front().sources.contains(id));
}

TEST_CASE("simple: changing a bus keeps the bus and the other members")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    REQUIRE(build_setup(backend, bus_plan(3)).has_value());
    auto found = setups(backend);
    REQUIRE_EQ(found.size(), std::size_t{1});
    REQUIRE(found[0].kind == job::bus);
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    const vss::link_id link_id = backend.links().front().id;
    const vss::endpoint_id first = found[0].members[0];
    const vss::endpoint_id second = found[0].members[1];
    const vss::endpoint_id third = found[0].members[2];

    // Add a member: the same bus, the members that were there stay.
    job_plan plan = plan_from(found[0], backend.ports());
    plan.first.push_back(end_plan{});
    REQUIRE(build_setup(backend, plan, &found[0]).has_value());
    found = setups(backend);
    REQUIRE_EQ(found.size(), std::size_t{1});
    REQUIRE(found[0].kind == job::bus);
    REQUIRE_EQ(found[0].members.size(), qsizetype{4});
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    REQUIRE_EQ(backend.links().front().id, link_id);
    REQUIRE(backend.links().front().sources.mid(0, 3) ==
            (QVector<vss::endpoint_id>{first, second, third}));
    REQUIRE_EQ(found[0].link, link_id);

    // Close a member: it goes, the others stay, and the bus is the bus.
    plan = plan_from(found[0], backend.ports());
    drop_first(plan, second);
    REQUIRE(build_setup(backend, plan, &found[0]).has_value());
    REQUIRE(!backend.port(second).has_value());
    found = setups(backend);
    REQUIRE_EQ(found.size(), std::size_t{1});
    REQUIRE_EQ(found[0].members.size(), qsizetype{3});
    REQUIRE(found[0].members.contains(first));
    REQUIRE(found[0].members.contains(third));
    REQUIRE_EQ(backend.links().front().id, link_id);
}

TEST_CASE("simple: a bus whose members all change is still the same bus")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    REQUIRE(build_setup(backend, bus_plan(2)).has_value());
    auto found = setups(backend);
    const vss::link_id link_id = backend.links().front().id;

    // Both members are replaced: the new ones join before the old ones leave,
    // so the bus never has fewer than two.
    job_plan plan = plan_from(found[0], backend.ports());
    plan.first = {end_plan{}, end_plan{}};
    REQUIRE(build_setup(backend, plan, &found[0]).has_value());
    found = setups(backend);
    REQUIRE_EQ(found.size(), std::size_t{1});
    REQUIRE(found[0].kind == job::bus);
    REQUIRE_EQ(found[0].members.size(), qsizetype{2});
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    REQUIRE_EQ(backend.links().front().id, link_id);
    REQUIRE_EQ(backend.ports().size(), std::size_t{2});
}

TEST_CASE("simple: a failed change of a bus leaves the setup as it was")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    REQUIRE(build_setup(backend, bus_plan(3)).has_value());
    const auto found = setups(backend);
    const vss::link_id link_id = backend.links().front().id;
    const std::size_t ports = backend.ports().size();
    const auto members = backend.links().front().sources;

    // A member is added and another taken off; the second step fails.
    job_plan plan = plan_from(found[0], backend.ports());
    plan.first.push_back(end_plan{});
    drop_first(plan, members[0]);
    backend.fail_after(2, vss::make_error_code(vss::errc::io_error));
    REQUIRE(!build_setup(backend, plan, &found[0]).has_value());
    REQUIRE_EQ(backend.ports().size(), ports);
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    REQUIRE_EQ(backend.links().front().id, link_id);
    REQUIRE(backend.links().front().sources == members);

    // Swapping two for two, the first removal fails: the new members that
    // joined are taken off again.
    plan = plan_from(found[0], backend.ports());
    drop_first(plan, members[0]);
    drop_first(plan, members[1]);
    plan.first.push_back(end_plan{});
    plan.first.push_back(end_plan{});
    backend.fail_after(4, vss::make_error_code(vss::errc::io_error));
    REQUIRE(!build_setup(backend, plan, &found[0]).has_value());
    REQUIRE_EQ(backend.ports().size(), ports);
    REQUIRE_EQ(backend.links().front().id, link_id);
    REQUIRE(backend.links().front().sources == members);
}

TEST_CASE("simple: a bus that would pass the most members is made again")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    const int most = static_cast<int>(vss::max_bus_members);
    REQUIRE(build_setup(backend, bus_plan(most)).has_value());
    auto found = setups(backend);
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    const vss::link_id link_id = backend.links().front().id;

    // Replacing one member keeps the bus at the most, which a bus edited in
    // place could not do (the new member would join first), so the bus is made
    // again.
    job_plan plan = plan_from(found[0], backend.ports());
    drop_first(plan, found[0].members[0]);
    plan.first.push_back(end_plan{});
    REQUIRE(build_setup(backend, plan, &found[0]).has_value());
    found = setups(backend);
    REQUIRE_EQ(found.size(), std::size_t{1});
    REQUIRE_EQ(found[0].members.size(), qsizetype{most});
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    REQUIRE(backend.links().front().id != link_id);
}

TEST_CASE("simple: echo answers a lone program port")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    job_plan plan;
    plan.kind = job::port;
    plan.first = {end_plan{}};
    plan.echo = true;
    REQUIRE(build_setup(backend, plan).has_value());
    const auto found = setups(backend);
    REQUIRE_EQ(found.size(), std::size_t{1});
    REQUIRE(found[0].kind == job::port);
    REQUIRE(found[0].echo != 0);
    REQUIRE(backend.echo(found[0].echo));
}

TEST_CASE("simple: the share form needs a device and creates the setup")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    JobForm empty(backend, job::share);
    REQUIRE(!empty.create_button()->isEnabled());
    REQUIRE(empty.blocker().contains(QStringLiteral("Choose a device")));

    with_devices(backend);
    JobForm form(backend, job::share);
    REQUIRE(form.create_button()->isEnabled());
    REQUIRE_EQ(form.plan().second.size(), std::size_t{2});
    form.set_label(true, 0, QStringLiteral("PuTTY"));
    REQUIRE(form.result_text().contains(QStringLiteral("COM3")));
    REQUIRE(form.result_text().contains(QStringLiteral("PuTTY")));
    REQUIRE(form.result_text().contains(QStringLiteral("not COM3")));

    vss::endpoint_id made = 0;
    QObject::connect(&form, &JobForm::done,
                     [&made](vss::endpoint_id port, const setup_prefs&)
                     { made = port; });
    REQUIRE(submit_and_wait(&form));
    REQUIRE(made != 0);
    const auto found = setups(backend);
    REQUIRE_EQ(found.size(), std::size_t{1});
    REQUIRE(found[0].kind == job::share);

    // The device in that setup is not offered again.
    JobForm next(backend, job::attach);
    REQUIRE_EQ(next.plan().first.front().device_path, QStringLiteral("COM4"));
}

TEST_CASE("simple: the form says when a COM name is chosen twice")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    JobForm form(backend, job::pair);
    REQUIRE(form.create_button()->isEnabled());
    REQUIRE(form.plan().first.front().com_number !=
            form.plan().second.front().com_number);
}

namespace
{

/// The COM numbers a form's port pickers offer, row by row.
std::vector<QStringList> offered_numbers(JobForm& form)
{
    std::vector<QStringList> rows;
    for (auto* combo :
         form.findChildren<QComboBox*>(QStringLiteral("jobFormPort")))
    {
        QStringList items;
        for (int index = 0; index < combo->count(); ++index)
            items << combo->itemText(index);
        rows.push_back(items);
    }
    return rows;
}

/// The visible failure note of a form, or an empty string.
QString failure_text(JobForm& form)
{
    for (auto* banner : form.findChildren<NoteBanner*>())
    {
        if (banner->isVisibleTo(&form) &&
            banner->text().startsWith(QStringLiteral("Could not")))
            return banner->text();
    }
    return {};
}

} // namespace

TEST_CASE("simple: numbers Windows keeps are neither offered nor chosen")
{
    // COM3 and COM4 are plugged in; COM5, COM6 and COM8 belong to ports that
    // are not listed (a removed port keeps its number).
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    with_devices(backend);
    backend.set_taken_com_numbers({5, 6, 8});

    JobForm form(backend, job::pair);
    REQUIRE_EQ(form.plan().first.front().com_number, 7u);
    REQUIRE_EQ(form.plan().second.front().com_number, 9u);
    const auto rows = offered_numbers(form);
    REQUIRE_EQ(rows.size(), std::size_t{2});
    for (const auto& items : rows)
    {
        for (const QString& gone :
             {QStringLiteral("COM3"), QStringLiteral("COM4"),
              QStringLiteral("COM5"), QStringLiteral("COM6"),
              QStringLiteral("COM8")})
            REQUIRE(!items.contains(gone));
    }
    // Each row keeps its own number in its list, the other row's is not there.
    REQUIRE(rows[0].contains(QStringLiteral("COM7")));
    REQUIRE(!rows[0].contains(QStringLiteral("COM9")));
    REQUIRE(rows[1].contains(QStringLiteral("COM9")));
    REQUIRE(!rows[1].contains(QStringLiteral("COM7")));

    REQUIRE(submit_and_wait(&form));
    const auto made = backend.ports();
    REQUIRE_EQ(made.size(), std::size_t{2});
    REQUIRE(std::any_of(made.begin(), made.end(), [](const port_view& port)
                        { return port.com_number == 7; }));
    REQUIRE(std::any_of(made.begin(), made.end(), [](const port_view& port)
                        { return port.com_number == 9; }));
}

TEST_CASE(
    "simple: no numbers are offered when Studio cannot tell which are free")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    backend.set_reservations_unknown(true);

    JobForm form(backend, job::pair);
    REQUIRE(offered_numbers(form).empty());
    REQUIRE_EQ(form.plan().first.front().com_number, 0u);
    REQUIRE_EQ(form.plan().second.front().com_number, 0u);
    REQUIRE(visible_texts(&form)
                .join(QLatin1Char('\n'))
                .contains(QStringLiteral("could not read which COM numbers "
                                         "Windows keeps")));
    // Studio picks the numbers, so the setup is still made.
    REQUIRE(submit_and_wait(&form));
    REQUIRE_EQ(backend.ports().size(), std::size_t{2});
}

TEST_CASE("simple: a refused number is named and replaced")
{
    // COM3 is held by a port the list of numbers does not show, so it is
    // offered and refused.
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    backend.set_hidden_com_numbers({3});

    JobForm form(backend, job::pair);
    REQUIRE_EQ(form.plan().first.front().com_number, 3u);
    REQUIRE_EQ(form.plan().second.front().com_number, 4u);
    REQUIRE(!submit_and_wait(&form));
    REQUIRE(backend.ports().empty());

    const QString failure = failure_text(form);
    REQUIRE(failure.startsWith(QStringLiteral("Could not set it up. COM3 "
                                              "already belongs to another "
                                              "device")));
    REQUIRE(failure.contains(QStringLiteral("Nothing was changed.")));
    REQUIRE(!failure.contains(QStringLiteral("already exists")));

    // The refused number is gone from both rows and the form holds a free one.
    REQUIRE_EQ(form.plan().first.front().com_number, 5u);
    REQUIRE_EQ(form.plan().second.front().com_number, 4u);
    // The old rows wait to be deleted; only the new ones count.
    settle();
    const auto rows = offered_numbers(form);
    REQUIRE_EQ(rows.size(), std::size_t{2});
    for (const auto& items : rows)
        REQUIRE(!items.contains(QStringLiteral("COM3")));
    REQUIRE(submit_and_wait(&form));
    REQUIRE_EQ(backend.ports().size(), std::size_t{2});
}

TEST_CASE("simple: a failure that is not a number reads as a sentence")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    JobForm form(backend, job::pair);
    backend.fail_next(vss::make_error_code(vss::errc::timeout));
    REQUIRE(!submit_and_wait(&form));
    const QString failure = failure_text(form);
    REQUIRE(failure.startsWith(QStringLiteral("Could not set it up. Studio "
                                              "reported: ")));
    REQUIRE(failure.endsWith(QStringLiteral("Nothing was changed.")));
}

TEST_CASE("simple: the form stays responsive and says it is working")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    backend.set_create_delay(std::chrono::milliseconds(300));
    JobForm form(backend, job::pair);
    form.show();
    settle();
    const QString idle_text = form.create_button()->text();
    int done_count = 0;
    QObject::connect(&form, &JobForm::done,
                     [&done_count](vss::endpoint_id, const setup_prefs&)
                     { ++done_count; });

    const auto started = std::chrono::steady_clock::now();
    REQUIRE(form.submit());
    // Returning at once is the point: the GUI thread is not held.
    REQUIRE(std::chrono::steady_clock::now() - started <
            std::chrono::milliseconds(250));
    REQUIRE(form.busy());
    REQUIRE(!form.create_button()->isEnabled());
    REQUIRE(form.create_button()->text() != idle_text);
    REQUIRE(form.create_button()->accessibleName() ==
            form.create_button()->text());
    auto* status = form.findChild<QLabel*>(QStringLiteral("jobFormBusyText"));
    REQUIRE(status != nullptr);
    REQUIRE(status->isVisibleTo(&form));
    REQUIRE(!status->accessibleName().isEmpty());

    // A second click or Enter while it runs starts nothing.
    REQUIRE(!form.submit());

    REQUIRE(vss_test::wait_until([&form] { return !form.busy(); }));
    REQUIRE_EQ(done_count, 1);
    REQUIRE_EQ(backend.ports().size(), std::size_t{2});
    REQUIRE(!status->isVisibleTo(&form));
    REQUIRE(form.create_button()->isEnabled());
    REQUIRE_EQ(form.create_button()->text(), idle_text);
}

TEST_CASE("simple: a failed build re-enables the form and says why")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    backend.set_create_delay(std::chrono::milliseconds(50));
    JobForm form(backend, job::pair);
    backend.fail_next(vss::make_error_code(vss::errc::timeout));
    REQUIRE(!submit_and_wait(&form));
    REQUIRE(form.create_button()->isEnabled());
    REQUIRE(!failure_text(form).isEmpty());
    REQUIRE(backend.ports().empty());
}

TEST_CASE("simple: closing the form while it works waits for the worker")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    backend.set_create_delay(std::chrono::milliseconds(100));
    {
        JobForm form(backend, job::pair);
        REQUIRE(form.submit());
    }
    // The worker finished before the form went, so nothing used a dead form.
    settle();
    REQUIRE_EQ(backend.ports().size(), std::size_t{2});
}

TEST_CASE("simple: building reports the COM number that clashed")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    backend.set_hidden_com_numbers({6});
    job_plan plan;
    plan.kind = job::pair;
    end_plan first;
    first.com_number = 5;
    end_plan second;
    second.com_number = 6;
    plan.first = {first};
    plan.second = {second};

    unsigned clashed = 99;
    const auto built = build_setup(backend, plan, nullptr, &clashed);
    REQUIRE(!built.has_value());
    REQUIRE_EQ(clashed, 6u);
    // What was made before the clash is taken away again.
    REQUIRE(backend.ports().empty());

    plan.second.front().com_number = 7;
    REQUIRE(build_setup(backend, plan, nullptr, &clashed).has_value());
    REQUIRE_EQ(clashed, 0u);
}

TEST_CASE("simple: Simple-mode words name no Studio part")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    with_devices(backend);
    const QRegularExpression parts(
        QStringLiteral("\\b(terminal|monitor|fixture|loopback|endpoint|"
                       "topology|link)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    JobGrid grid(backend);
    for (const QString& text : visible_texts(&grid))
    {
        if (parts.match(text).hasMatch())
            throw std::runtime_error("Studio word on the grid: " +
                                     text.toStdString());
    }
    for (const job kind :
         {job::port, job::pair, job::attach, job::share, job::talk,
          job::network, job::bus, job::combine, job::turns})
    {
        JobForm form(backend, kind);
        for (const QString& text : visible_texts(&form))
        {
            if (parts.match(text).hasMatch())
                throw std::runtime_error("Studio word in a form: " +
                                         text.toStdString());
        }
        QString result = form.result_text();
        REQUIRE(!parts.match(result).hasMatch());
    }
}

TEST_CASE(
    "simple: Create stays in reach at 200 percent text and a small window")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    with_devices(backend);
    JobForm form(backend, job::share);
    form.resize(560, 360);
    form.show();
    vss_test::scale_text(&form, 2.0);
    QApplication::processEvents();
    QPushButton* create = form.create_button();
    REQUIRE(create->isVisible());
    const QRect inside(create->mapTo(&form, QPoint(0, 0)), create->size());
    REQUIRE(form.rect().contains(inside));
}

TEST_CASE(
    "simple: a setup page tells what to open and lets you type to the device")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    with_devices(backend);
    REQUIRE(build_setup(backend, share_plan(2)).has_value());
    const auto found = setups(backend);
    setup_prefs prefs;
    prefs.type_here = true;
    SetupPage page(backend, found[0], prefs);
    REQUIRE(page.steps_text().contains(QStringLiteral("In PuTTY, open")));
    REQUIRE(page.steps_text().contains(QStringLiteral("COM5")));
    REQUIRE(page.steps_text().contains(QStringLiteral("Do not open")));
    REQUIRE_EQ(page.type_target(), found[0].device);
    REQUIRE(page.send_typed(QStringLiteral("AT")));
    REQUIRE_EQ(backend.sent().back(), QByteArray("AT\r\n"));
    REQUIRE_EQ(backend.sent_to().back(), found[0].device);
    REQUIRE(page.received_text().contains(QStringLiteral("AT")));

    // A program on one of the ports shows up as such.
    backend.set_peer_state(found[0].programs[0], peer_state::open);
    backend.set_peer_state(found[0].programs[1], peer_state::not_open);
    page.refresh(setups(backend)[0]);
    const QString lines = page.port_lines().join(QLatin1Char('\n'));
    REQUIRE(lines.contains(QStringLiteral("Open in a program")));
    REQUIRE(lines.contains(QStringLiteral("No program yet")));
}

TEST_CASE("simple: the page lists setups and opens the one just made")
{
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    with_devices(backend);
    SimplePage page(backend);
    page.resize(1100, 760);
    REQUIRE_EQ(page.list()->count(), 1); // the empty line
    page.open_job(job::talk);
    REQUIRE(page.form() != nullptr);
    REQUIRE(submit_and_wait(page.form()));
    QApplication::processEvents();
    REQUIRE_EQ(page.setups().size(), std::size_t{1});
    REQUIRE(page.setup_page() != nullptr);
    REQUIRE(page.setup_page()->type_target() != 0);
    REQUIRE(
        page.list()->item(0)->text().startsWith(QStringLiteral("Trying COM3")));

    // Add a program through the page: talk has none, so make a share.
    page.open_job(job::share);
    REQUIRE(submit_and_wait(page.form()));
    QApplication::processEvents();
    REQUIRE_EQ(page.setups().size(), std::size_t{2});
}

TEST_CASE("simple: setup rows have padding around their text")
{
    Theme::instance().apply();
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    with_devices(backend);
    REQUIRE(build_setup(backend, share_plan(2)).has_value());
    SimplePage page(backend);
    page.resize(1100, 760);
    page.show();
    settle();
    QListWidget* list = page.list();
    REQUIRE(list->count() > 0);
    for (const double scale : {1.0, 2.0})
    {
        vss_test::scale_text(&page, scale);
        settle();
        const QString text = list->item(0)->text();
        const int lines = static_cast<int>(text.count(QLatin1Char('\n'))) + 1;
        const int pad = 2 * Theme::instance().space(0);
        const int bare = lines * list->fontMetrics().lineSpacing();
        REQUIRE(list->sizeHintForRow(0) >= bare + pad);
        REQUIRE(list->sizeHintForRow(0) >= 24);
    }
}

TEST_CASE("simple: pictures for a person to review")
{
    // Writes PNGs only when asked, for the visual check in AGENTS.md; they
    // are never committed.
    const QString folder = qEnvironmentVariable("VSS_SIMPLE_GRAB");
    if (folder.isEmpty())
        VSS_SKIP("set VSS_SIMPLE_GRAB to a folder to write the pictures");
    FakeStudioBackend backend;
    backend.set_capabilities(windows_caps());
    with_devices(backend);
    SimplePage page(backend);
    page.resize(1180, 820);
    page.show();
    settle();
    page.grab().save(folder + QStringLiteral("/1-jobs.png"));
    page.open_job(job::share);
    page.form()->set_label(true, 0, QStringLiteral("PuTTY"));
    page.form()->set_label(true, 1, QStringLiteral("Data logger"));
    page.form()->set_extras(true, true);
    settle();
    page.grab().save(folder + QStringLiteral("/2-share-form.png"));
    REQUIRE(submit_and_wait(page.form()));
    settle();
    const auto found = setups(backend);
    backend.set_peer_state(found[0].programs[0], peer_state::open);
    backend.set_peer_state(found[0].programs[1], peer_state::not_open);
    page.refresh();
    settle();
    page.grab().save(folder + QStringLiteral("/3-share-page.png"));
    page.open_job(job::port);
    settle();
    page.grab().save(folder + QStringLiteral("/4-port-form.png"));
}
