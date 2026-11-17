//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The helper service that makes the ports of the user-mode driver: the rules it
// keeps (which slot, how many for one user, how many in all) and the protocol
// that the studio, the helper and the driver share. The service itself needs a
// machine with the driver; the guest scripts run it.
//

#include "test_framework.hpp"

#include <cstddef>
#include <string>

// windows.h first: devpropdef.h needs its types.
// clang-format off
#include <windows.h>
#include <devpropdef.h>
// clang-format on

#include <vsscom_helper/com_number.hpp>
#include <vsscom_helper/lease_table.hpp>
#include <vsscom_helper/protocol.h>

using vss::helper::lease_status;
using vss::helper::lease_table;

TEST_CASE("helper: the first slot is zero and the lowest free slot is next")
{
    lease_table table(4, 4);

    const auto first = table.acquire("alice");
    REQUIRE(first.status == lease_status::granted);
    REQUIRE_EQ(first.slot, std::size_t{0});

    const auto second = table.acquire("alice");
    REQUIRE_EQ(second.slot, std::size_t{1});
    REQUIRE_EQ(table.count(), std::size_t{2});

    // A slot that is given back is the next one given, so a user who makes a
    // port again gets the same COM name.
    REQUIRE(table.release("alice", 0));
    const auto again = table.acquire("alice");
    REQUIRE(again.status == lease_status::granted);
    REQUIRE_EQ(again.slot, std::size_t{0});
}

TEST_CASE("helper: one user cannot hold more than their share")
{
    lease_table table(8, 2);

    REQUIRE(table.acquire("alice").status == lease_status::granted);
    REQUIRE(table.acquire("alice").status == lease_status::granted);
    REQUIRE(table.acquire("alice").status == lease_status::owner_quota);
    REQUIRE_EQ(table.count_of("alice"), std::size_t{2});

    // Another user is not held up by it.
    REQUIRE(table.acquire("bob").status == lease_status::granted);
    REQUIRE_EQ(table.count_of("bob"), std::size_t{1});
}

TEST_CASE("helper: all users together cannot take more slots than there are")
{
    lease_table table(3, 2);

    REQUIRE(table.acquire("alice").status == lease_status::granted);
    REQUIRE(table.acquire("alice").status == lease_status::granted);
    REQUIRE(table.acquire("bob").status == lease_status::granted);

    // Full for bob, who is under his share: the machine is out of slots.
    REQUIRE(table.acquire("bob").status == lease_status::no_slots);
    // Alice is at her share, and hears that first.
    REQUIRE(table.acquire("alice").status == lease_status::owner_quota);
}

TEST_CASE("helper: only the owner gives a slot back")
{
    lease_table table(4, 4);
    const auto grant = table.acquire("alice");

    REQUIRE(!table.release("bob", grant.slot));
    REQUIRE_EQ(table.count(), std::size_t{1});

    REQUIRE(table.release("alice", grant.slot));
    REQUIRE_EQ(table.count(), std::size_t{0});

    // A slot that is free, or that does not exist, is not released.
    REQUIRE(!table.release("alice", grant.slot));
    REQUIRE(!table.release("alice", 99));
}

TEST_CASE("helper: an owner is any string, and binary ones differ")
{
    // The helper uses the bytes of a SID, which can hold zeros.
    lease_table table(4, 1);
    const std::string first("\x01\x00\x05", 3);
    const std::string second("\x01\x00\x06", 3);

    REQUIRE(table.acquire(first).status == lease_status::granted);
    REQUIRE(table.acquire(first).status == lease_status::owner_quota);
    REQUIRE(table.acquire(second).status == lease_status::granted);
}

TEST_CASE("helper: the protocol has the layout the studio and the helper share")
{
    // The client and the helper are built apart; a change here is a new
    // version.
    REQUIRE_EQ(sizeof(VSSCOM_HELPER_REQUEST), std::size_t{16});
    REQUIRE_EQ(sizeof(VSSCOM_HELPER_REPLY), std::size_t{16});
    REQUIRE_EQ(offsetof(VSSCOM_HELPER_REQUEST, Op), std::size_t{4});
    REQUIRE_EQ(offsetof(VSSCOM_HELPER_REQUEST, Slot), std::size_t{8});
    REQUIRE_EQ(offsetof(VSSCOM_HELPER_REPLY, Status), std::size_t{4});
    REQUIRE_EQ(offsetof(VSSCOM_HELPER_REPLY, Slot), std::size_t{8});
    REQUIRE_EQ(static_cast<int>(VSSCOM_HELPER_VERSION), 1);
    REQUIRE_EQ(static_cast<int>(VSSCOM_HELPER_OP_CREATE), 1);
    REQUIRE_EQ(static_cast<int>(VSSCOM_HELPER_OP_DESTROY), 2);
    REQUIRE_EQ(static_cast<int>(VSSCOM_HELPER_OP_SET_COM_NUMBER), 3);
    REQUIRE_EQ(static_cast<int>(VSSCOM_HELPER_OP_SET_COM_NUMBER_KM), 4);
    REQUIRE_EQ(offsetof(VSSCOM_HELPER_REQUEST, Argument), std::size_t{12});
    REQUIRE_EQ(static_cast<int>(VSSCOM_HELPER_MIN_COM_NUMBER), 1);
    REQUIRE_EQ(static_cast<int>(VSSCOM_HELPER_MAX_COM_NUMBER), 255);

    // The limits are the control device's, so the two drivers behave alike.
    REQUIRE_EQ(static_cast<int>(VSSCOM_HELPER_MAX_PORTS), 32);
    REQUIRE_EQ(static_cast<int>(VSSCOM_HELPER_MAX_PORTS_PER_USER), 16);
    REQUIRE_EQ(static_cast<std::size_t>(VSSCOM_HELPER_MAX_SID_BYTES),
               static_cast<std::size_t>(SECURITY_MAX_SID_SIZE));
}

TEST_CASE("helper: the names the guest scripts and the service use")
{
    // install-helper.ps1 and test-driver.ps1 spell these out.
    REQUIRE(std::wstring(VssComHelperPipeName) ==
            L"\\\\.\\pipe\\vsscom_helper");
    REQUIRE(std::wstring(VssComHelperServiceName) == L"vsscom_helper");
    REQUIRE(std::wstring(VssComUmPortHardwareId) == L"VSSCOMUM\\PORT");
    REQUIRE(std::wstring(VssComUmPortInstancePrefix) == L"SWD\\VSSCOMUM\\");
    REQUIRE_EQ(VssComUmOwnerKey.pid, static_cast<ULONG>(2));
}

TEST_CASE("helper: the COM numbers a port may have")
{
    using vss::helper::com_number_valid;

    // The range of protocol.h, which the studio and the helper both enforce.
    REQUIRE(!com_number_valid(0));
    REQUIRE(com_number_valid(1));
    REQUIRE(com_number_valid(255));
    REQUIRE(!com_number_valid(256));
    REQUIRE_EQ(vss::helper::min_com_number,
               static_cast<unsigned long>(VSSCOM_HELPER_MIN_COM_NUMBER));
    REQUIRE_EQ(vss::helper::max_com_number,
               static_cast<unsigned long>(VSSCOM_HELPER_MAX_COM_NUMBER));
}

TEST_CASE("helper: COM names read and write the way Windows spells them")
{
    using vss::helper::com_name;
    using vss::helper::parse_com_name;

    REQUIRE(com_name(1) == L"COM1");
    REQUIRE(com_name(255) == L"COM255");

    REQUIRE_EQ(parse_com_name(L"COM7").value_or(0), 7ul);
    REQUIRE_EQ(parse_com_name(L"com12").value_or(0), 12ul);
    REQUIRE_EQ(parse_com_name(L"Com255").value_or(0), 255ul);

    REQUIRE(!parse_com_name(L"").has_value());
    REQUIRE(!parse_com_name(L"COM").has_value());
    REQUIRE(!parse_com_name(L"COM-1").has_value());
    REQUIRE(!parse_com_name(L"COMX").has_value());
    REQUIRE(!parse_com_name(L"COM7x").has_value());
    REQUIRE(!parse_com_name(L"LPT1").has_value());
    REQUIRE(!parse_com_name(L"CNCA0").has_value());
    REQUIRE(!parse_com_name(L"COM12345678").has_value());
}

TEST_CASE("helper: the friendly name follows the number")
{
    using vss::helper::friendly_name_for;

    // What the Ports class installer made of it, changed as Device Manager would.
    REQUIRE(friendly_name_for(L"Virtual Serial Studio COM Port (COM4)", 9) ==
            L"Virtual Serial Studio COM Port (COM9)");
    REQUIRE(friendly_name_for(L"Virtual Serial Studio COM Port (COM4)", 4) ==
            L"Virtual Serial Studio COM Port (COM4)");
    REQUIRE(friendly_name_for(L"Port (COM12) ", 3) == L"Port (COM3)");
    // A name with another bracket is kept whole, and gets the COM name added.
    REQUIRE(friendly_name_for(L"Adapter (USB)", 5) == L"Adapter (USB) (COM5)");
    REQUIRE(friendly_name_for(L"Adapter", 5) == L"Adapter (COM5)");
    // None at all: just the COM name.
    REQUIRE(friendly_name_for(L"", 5) == L"(COM5)");
}

TEST_CASE("helper: a number held by nothing, or only by Studio ports that are gone, may be taken")
{
    using vss::helper::holder_kind;
    using vss::helper::number_holder;
    using vss::helper::plan_claim;

    // Only the COM name database marks it: nothing owns it.
    auto plan = plan_claim(false, {});
    REQUIRE(plan.allowed);
    REQUIRE(plan.remove.empty());

    // Studio ports that were removed keep the number; they are cleared away.
    plan = plan_claim(false,
                      {number_holder{holder_kind::studio_ghost,
                                     L"SWD\\VSSCOMUM\\0003"},
                       number_holder{holder_kind::studio_ghost,
                                     L"SWD\\VSSCOMUM\\0009"}});
    REQUIRE(plan.allowed);
    REQUIRE_EQ(plan.remove.size(), std::size_t{2});
    REQUIRE(plan.remove[0] == L"SWD\\VSSCOMUM\\0003");
}

TEST_CASE("helper: a number that something else has is never taken")
{
    using vss::helper::holder_kind;
    using vss::helper::number_holder;
    using vss::helper::plan_claim;

    // The name resolves: a real port, com0com, a running port.
    REQUIRE(!plan_claim(true, {}).allowed);
    REQUIRE(!plan_claim(true, {number_holder{holder_kind::studio_ghost,
                                             L"SWD\\VSSCOMUM\\0003"}})
                 .allowed);
    // A present device holds it.
    REQUIRE(!plan_claim(false, {number_holder{holder_kind::live_device,
                                              L"USB\\VID_1&PID_2\\3"}})
                 .allowed);
    // An unplugged port of something else comes back to its number.
    REQUIRE(!plan_claim(false, {number_holder{holder_kind::other_ghost,
                                              L"USB\\VID_1&PID_2\\3"}})
                 .allowed);
    // One owner that is not Studio's spoils it even beside a Studio ghost.
    const auto plan = plan_claim(
        false, {number_holder{holder_kind::studio_ghost, L"SWD\\VSSCOMUM\\0003"},
                number_holder{holder_kind::other_ghost, L"USB\\X\\1"}});
    REQUIRE(!plan.allowed);
    REQUIRE(plan.remove.empty());
}

TEST_CASE("helper: which instance ids are Studio's ports")
{
    using vss::helper::is_studio_port_instance;
    REQUIRE(is_studio_port_instance(L"SWD\\VSSCOMUM\\0003"));
    REQUIRE(is_studio_port_instance(L"swd\\vsscomum\\000A"));
    REQUIRE(is_studio_port_instance(L"VSSCOM\\PORT\\0001"));
    REQUIRE(!is_studio_port_instance(L"USB\\VID_303A&PID_1001&MI_00\\8&1"));
    REQUIRE(!is_studio_port_instance(L"COM0COM\\PORT\\CNCA1"));
    REQUIRE(!is_studio_port_instance(L""));
}
