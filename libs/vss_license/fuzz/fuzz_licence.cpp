//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fuzz harness for licence files. A licence file is whatever a person chose
// to import, so the readers must refuse bad input with an error and never
// crash. The signature keeps a fuzzer away from the payload readers, so the
// same bytes are also given to them directly. What a reader accepts must
// write back and read again as the same value.
//

#include <vss/license/license.hpp>
#include <vss/license/signed_file.hpp>
#include <vss/license/test_keys.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>

// libFuzzer looks for exactly this name.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size)
{
    namespace lic = vss::license;
    namespace sf = vss::license::signed_file;

    const std::string_view text(reinterpret_cast<const char*>(data), size);
    const std::span<const std::uint8_t> bytes(data, size);

    if (const auto file = sf::parse(text))
    {
        const auto again = sf::parse(sf::write(*file));
        if (!again || again->payload != file->payload ||
            again->sig != file->sig || again->purpose != file->purpose ||
            again->key_id != file->key_id)
        {
            std::abort();
        }
    }

    lic::context where;
    where.now = lic::time_point{std::chrono::sys_days{
        std::chrono::year{2027} / std::chrono::January / 1}};
    where.computer_name = "LAB-RIG-07";
    where.keys = lic::test_keys::trusted();
    const lic::status judged = lic::evaluate(text, where);
    static_cast<void>(lic::check(judged, "plugins.official"));
    static_cast<void>(lic::read_revocations(text, where.keys));

    if (const auto value = lic::parse_payload(bytes))
    {
        const auto written = lic::write_payload(*value);
        if (!written)
        {
            std::abort();
        }
        const lic::status direct = lic::evaluate(*value, nullptr, where);
        static_cast<void>(lic::check(direct, "support"));
    }

    if (const auto list = lic::parse_revocations(bytes))
    {
        if (!lic::write_revocations(*list))
        {
            std::abort();
        }
    }

    static_cast<void>(sf::base64url_decode(text));
    static_cast<void>(lic::parse_time(text));
    return 0;
}
