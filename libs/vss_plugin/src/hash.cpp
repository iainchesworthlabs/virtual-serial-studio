//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/hash.hpp>

#include <boost/hash2/sha2.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace vss::plugin
{

namespace
{

constexpr std::string_view hex_digits = "0123456789abcdef";

void set_detail(std::string* detail, std::string text)
{
    if (detail != nullptr)
    {
        *detail = std::move(text);
    }
}

result<void> refuse(plugin_errc code, std::string* detail, std::string text)
{
    set_detail(detail, std::move(text));
    return make_error_code(code);
}

std::string in_quotes(std::string_view text)
{
    std::string out;
    out.reserve(text.size() + 2);
    out += '"';
    out += text;
    out += '"';
    return out;
}

bool is_own_file(std::string_view path) noexcept
{
    return path == manifest_file_name || path == signature_file_name;
}

/// The measured files in the order of their folded paths, so that a path
/// and a clash are both found by looking at neighbours.
std::vector<std::pair<std::string, const file_entry*>>
by_folded_path(std::span<const file_entry> files)
{
    std::vector<std::pair<std::string, const file_entry*>> sorted;
    sorted.reserve(files.size());
    for (const file_entry& file : files)
    {
        sorted.emplace_back(fold_path(file.path), &file);
    }
    std::ranges::sort(sorted, [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    return sorted;
}

const file_entry*
find_exact(const std::vector<std::pair<std::string, const file_entry*>>& sorted,
           std::string_view path)
{
    const std::string folded = fold_path(path);
    const auto found = std::ranges::lower_bound(
        sorted, folded, {}, [](const auto& item) -> const std::string& {
            return item.first;
        });
    if (found == sorted.end() || found->first != folded ||
        found->second->path != path)
    {
        return nullptr;
    }
    return found->second;
}

} // namespace

struct sha512_hasher::state
{
    boost::hash2::sha2_512 hash;
};

sha512_hasher::sha512_hasher()
    : state_(std::make_unique<state>())
{
}

sha512_hasher::~sha512_hasher() = default;

void sha512_hasher::update(std::span<const std::byte> bytes)
{
    // An empty span may carry a null pointer, which the hash passes to memcpy.
    if (bytes.empty())
    {
        return;
    }
    state_->hash.update(bytes.data(), bytes.size());
}

void sha512_hasher::update(std::string_view text)
{
    if (text.empty())
    {
        return;
    }
    state_->hash.update(text.data(), text.size());
}

sha512_digest sha512_hasher::finish()
{
    const auto result = state_->hash.result();
    sha512_digest digest{};
    std::ranges::copy(result, digest.begin());
    return digest;
}

sha512_digest sha512_of(std::span<const std::byte> bytes)
{
    sha512_hasher hasher;
    hasher.update(bytes);
    return hasher.finish();
}

sha512_digest sha512_of(std::string_view text)
{
    sha512_hasher hasher;
    hasher.update(text);
    return hasher.finish();
}

std::string to_hex(const sha512_digest& digest)
{
    std::string text;
    text.reserve(digest.size() * 2);
    for (const std::uint8_t byte : digest)
    {
        const std::size_t value = byte;
        text += hex_digits[value >> 4U];
        text += hex_digits[value & 0x0fU];
    }
    return text;
}

std::string plugin_hash::short_text() const
{
    return to_hex(bytes).substr(0, 16);
}

plugin_hash hash_plugin(std::span<const file_entry> files)
{
    std::vector<const file_entry*> sorted;
    sorted.reserve(files.size());
    for (const file_entry& file : files)
    {
        sorted.push_back(&file);
    }
    // Paths are compared as bytes; the size and the digest only decide the
    // order of two entries with one path, which `check_files` refuses, so
    // that the hash never depends on the order the files were found in.
    std::ranges::sort(sorted,
                      [](const file_entry* left, const file_entry* right) {
                          if (left->path != right->path)
                          {
                              return left->path < right->path;
                          }
                          if (left->size != right->size)
                          {
                              return left->size < right->size;
                          }
                          return left->sha512 < right->sha512;
                      });

    constexpr std::string_view zero("\0", 1);
    sha512_hasher hasher;
    for (const file_entry* file : sorted)
    {
        hasher.update(file->path);
        hasher.update(zero);
        hasher.update(std::to_string(file->size));
        hasher.update(zero);
        hasher.update(to_hex(file->sha512));
        hasher.update("\n");
    }
    return plugin_hash{hasher.finish()};
}

result<void> check_files(const manifest& declared,
                         std::span<const file_entry> measured,
                         std::string* detail)
{
    if (detail != nullptr)
    {
        detail->clear();
    }
    if (measured.size() > max_files + 2)
    {
        return refuse(plugin_errc::layout_invalid, detail,
                      "the plugin has more files than the limit of " +
                          std::to_string(max_files));
    }
    for (const file_entry& file : measured)
    {
        if (const std::optional<std::string> wrong =
                check_relative_path(file.path))
        {
            return refuse(plugin_errc::layout_invalid, detail,
                          "file " + in_quotes(file.path) + ": " + *wrong);
        }
    }

    const auto sorted = by_folded_path(measured);
    for (std::size_t i = 1; i < sorted.size(); ++i)
    {
        if (sorted[i].first == sorted[i - 1].first)
        {
            const std::string_view first = sorted[i - 1].second->path;
            const std::string_view second = sorted[i].second->path;
            return refuse(plugin_errc::layout_invalid, detail,
                          first == second
                              ? "file " + in_quotes(first) + " is there twice"
                              : "files " + in_quotes(first) + " and " +
                                    in_quotes(second) +
                                    " differ only in case");
        }
    }

    if (find_exact(sorted, manifest_file_name) == nullptr)
    {
        return refuse(plugin_errc::layout_invalid, detail,
                      "the plugin has no " + std::string(manifest_file_name));
    }
    if (find_exact(sorted, declared.run.executable) == nullptr)
    {
        return refuse(plugin_errc::file_missing, detail,
                      "file " + in_quotes(declared.run.executable) +
                          ", which the manifest runs, is not there");
    }
    if (declared.files.empty())
    {
        return {};
    }

    for (const file_entry& listed : declared.files)
    {
        const file_entry* found = find_exact(sorted, listed.path);
        if (found == nullptr)
        {
            return refuse(plugin_errc::file_missing, detail,
                          "file " + in_quotes(listed.path) +
                              ", which the manifest lists, is not there");
        }
        if (found->size != listed.size)
        {
            return refuse(plugin_errc::signature_invalid, detail,
                          "file " + in_quotes(listed.path) + " is " +
                              std::to_string(found->size) +
                              " bytes and the manifest says " +
                              std::to_string(listed.size));
        }
        if (found->sha512 != listed.sha512)
        {
            return refuse(plugin_errc::signature_invalid, detail,
                          "file " + in_quotes(listed.path) +
                              " is not the file the manifest lists");
        }
    }

    // Every listed file was found by its exact path and the measured paths
    // are all different, so the counts differ only by files not listed.
    const auto listed_sorted = by_folded_path(declared.files);
    for (const file_entry& file : measured)
    {
        if (is_own_file(file.path))
        {
            continue;
        }
        if (find_exact(listed_sorted, file.path) == nullptr)
        {
            return refuse(plugin_errc::unlisted_file, detail,
                          "file " + in_quotes(file.path) +
                              " is not listed in the manifest");
        }
    }
    return {};
}

bool is_reserved_id(std::string_view id) noexcept
{
    return id.starts_with(reserved_id_prefix);
}

} // namespace vss::plugin
