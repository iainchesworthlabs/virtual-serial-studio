//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The licence test vectors of sdk/licence-reader/vectors: the licence files,
// what each is read with, and the answer a reader must give. They are made
// here, with the published RFC 8032 test key and no other, and the files in
// that folder are what this makes; a unit test fails when the two differ.
// The expected answers are written down here from docs/licensing.md and are
// not taken from either reader.
//

#pragma once

#include <string>
#include <vector>

namespace vss_test_support::licence_vectors
{

/// One key of a key list. The public key is always that of the test key;
/// only the id and the window differ.
struct vector_key
{
    std::string id;
    /// A time, or empty for no bound.
    std::string not_before;
    std::string not_after;
};

struct key_list
{
    std::string name;
    std::vector<vector_key> keys;
};

struct licence_vector
{
    /// The file is `<name>.vsslic`.
    std::string name;
    std::string about;
    std::string file_text;
    /// The name of the key list to read it with.
    std::string keys = "test";
    std::string now;
    /// Lower bounds for the clock check, or empty.
    std::string build_time;
    std::string latest_seen;
    /// The licence grants the answer when it names any one of these.
    std::vector<std::string> features{"plugins.official"};
    /// When the thing asked about was released, or empty.
    std::string released;
    /// Where the licence lists computers: a name that is on its list.
    /// Studio's library checks it; the reference reader has no such check.
    std::string computer_name;

    // The answer.
    std::string reason;
    bool granted = false;
    /// Empty unless the signature verified and the payload was read.
    std::string licence_id;
    std::string state;
};

/// The key lists the vectors name.
std::vector<key_list> key_lists();

/// Every vector, in the order of the index.
std::vector<licence_vector> all();

/// The text of `index.json`.
std::string index_text();

/// The public half of the test key, as 64 hexadecimal digits.
std::string test_public_key_hex();

} // namespace vss_test_support::licence_vectors
