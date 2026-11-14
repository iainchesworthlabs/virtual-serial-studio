//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A throwaway key pair for the tests and for the test mode of the issuing
// tool. It is the key of test vector 1 of RFC 8032, whose private half is
// printed in the RFC, so it protects nothing. Its key id starts with `test-`,
// which `signed_file::product_trust_list` never returns. Products do not link
// this part (vss_license_test_keys).
//

#pragma once

#include <vss/license/sign.hpp>
#include <vss/license/signed_file.hpp>

#include <span>
#include <string_view>

namespace vss::license::test_keys
{

inline constexpr std::string_view key_id = "test-rfc8032-1";

/// THROWAWAY TEST KEY. The private key of RFC 8032, section 7.1, test 1.
signing::seed private_key();

/// The test key as a trusted key for the `licence` and `revocations`
/// purposes.
std::span<const signed_file::trusted_key> trusted();

} // namespace vss::license::test_keys
