//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/install.hpp>

#include <vss/plugin/access.hpp>
#include <vss/plugin/discover.hpp>
#include <vss/plugin/hash.hpp>
#include <vss/plugin/licence.hpp>
#include <vss/plugin/measure.hpp>

#include "official_key_list.hpp"
#include "reader.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace vss::plugin
{

namespace
{

namespace fs = std::filesystem;

constexpr std::size_t copy_chunk = std::size_t{64} * 1024;

void set_detail(std::string* detail, std::string text)
{
    if (detail != nullptr)
        *detail = std::move(text);
}

error_code refuse(plugin_errc code, std::string* detail, std::string text)
{
    set_detail(detail, std::move(text));
    return make_error_code(code);
}

error_code refuse(errc code, std::string* detail, std::string text)
{
    set_detail(detail, std::move(text));
    return make_error_code(code);
}

/// The path as UTF-8 text with `/` between its names.
std::string shown(const fs::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

error_code from_std(const std::error_code& ec)
{
    if (!ec)
        return {};
    return error_code(ec.value(), boost::system::system_category());
}

// -- inspect ----------------------------------------------------------------

/// Reads `plugin.sig`.  `absent` when it is not there; a problem when it is a
/// link or too large.
struct signature_file
{
    std::optional<std::string> bytes;
    error_code problem;
    std::string detail;
};

signature_file read_signature_file(const fs::path& directory)
{
    signature_file out;
    const std::string name(signature_file_name);
    std::string detail;
    result<std::string> read =
        read_plugin_file(directory / name, max_signature_size, &detail);
    if (read.has_value())
    {
        out.bytes = std::move(read).value();
        return out;
    }
    if (read.error() == plugin_errc::file_missing)
        return out;
    out.problem = read.error();
    out.detail = detail;
    return out;
}

bool official_blocks(const error_code& run) noexcept
{
    return run == plugin_errc::reserved_id || run == plugin_errc::revoked ||
           run == plugin_errc::signature_invalid ||
           run == plugin_errc::manifest_invalid;
}

inspection inspect(const fs::path& directory, const inspect_context& context)
{
    inspection out;
    out.directory = directory;
    std::string detail;

    // plugin.json is read once; everything after works on these bytes.
    const std::string manifest_name(manifest_file_name);
    result<std::string> bytes =
        read_plugin_file(directory / manifest_name, max_manifest_size, &detail);
    if (bytes.has_error())
    {
        out.problem = bytes.error();
        out.detail = detail;
        return out;
    }
    out.manifest_bytes = std::move(bytes).value();

    result<manifest> parsed = parse_manifest(out.manifest_bytes, &detail);
    if (parsed.has_error())
    {
        out.problem = parsed.error();
        out.detail = detail;
        return out;
    }
    out.declared = std::move(parsed).value();
    const manifest& declared = *out.declared;

    result<std::vector<file_entry>> measured =
        measure_directory(directory, &detail);
    if (measured.has_error())
    {
        out.problem = measured.error();
        out.detail = detail;
        return out;
    }
    out.files = std::move(measured).value();
    out.hash = hash_plugin(out.files);

    const result<void> checked = check_files(declared, out.files, &detail);
    if (checked.has_error())
    {
        out.problem = checked.error();
        out.detail = detail;
    }

    signature_file sig = read_signature_file(directory);
    if (sig.problem.failed() && !out.problem.failed())
    {
        out.problem = sig.problem;
        out.detail = sig.detail;
    }

    // The signature is looked at even when a file is wrong, so that the
    // answer can name the signer.  A signature that does not hold is an
    // answer of its own.
    signature_input input;
    input.manifest_bytes = out.manifest_bytes;
    input.declared = &declared;
    if (sig.bytes.has_value())
        input.signature_text = std::string_view(*sig.bytes);
    input.official = context.official ? &*context.official : nullptr;
    input.administrator_keys = context.administrator_keys;
    input.user_keys = context.user_keys;
    input.today = context.today;
    out.signature = check_signature(input, &out.signature_detail);

    if (out.problem.failed())
        return out;
    if (out.signature.state == signature_state::invalid)
    {
        out.problem = make_error_code(plugin_errc::signature_invalid);
        out.detail = out.signature_detail;
        return out;
    }

    verify_input what;
    what.declared = &declared;
    what.hash = out.hash;
    what.signature = out.signature;
    what.revocations = context.revocations ? &*context.revocations : nullptr;
    what.rules = context.rules ? &*context.rules : nullptr;
    what.today = context.today;
    what.host_api = context.host_api;
    what.capabilities = context.capabilities;
    what.sandbox = context.sandbox;
    what.install = context.install;
    if (context.trust.has_value())
    {
        const plugin_decision* decision = context.trust->find(
            declared.id, out.hash, out.signature.key_id);
        what.sandbox_override =
            decision != nullptr && decision->run_without_sandbox &&
            may_override_sandbox(what.rules, context.install);
    }

    std::string run_detail;
    const result<verify_result> decided = verify(what, &run_detail);
    if (decided.has_error())
    {
        out.run = decided.error();
        out.run_detail = run_detail;
        // `verify` gives no level with a refusal.  The level a signature
        // gives is worked out the same way for the report.
        switch (out.signature.state)
        {
        case signature_state::valid:
            out.level = out.signature.origin == key_origin::official
                            ? trust_level::official
                            : trust_level::third_party_signed;
            break;
        case signature_state::unknown_key:
            out.level = trust_level::signed_unknown_key;
            break;
        default:
            out.level = trust_level::unsigned_plugin;
            break;
        }
        out.key_id = out.signature.key_id;
    }
    else
    {
        out.verified = decided.value();
        out.level = decided.value().level;
        out.key_id = decided.value().key_id;
    }

    if (context.licence_known && out.level.has_value() &&
        needs_licence(*out.level, declared))
    {
        out.licence_checked = true;
        const result<void> answer = check_licence(*out.level, declared,
                                                  context.licence,
                                                  &out.licence);
        if (answer.has_error())
            out.licence_problem = answer.error();
    }
    return out;
}

// -- names ------------------------------------------------------------------

constexpr std::array<std::string_view, 4> device_names{"con", "prn", "aux",
                                                       "nul"};

bool is_device_name(std::string_view word) noexcept
{
    if (std::ranges::find(device_names, word) != device_names.end())
        return true;
    if (word.size() == 4 && (word.substr(0, 3) == "com" ||
                             word.substr(0, 3) == "lpt"))
        return word[3] >= '0' && word[3] <= '9';
    return false;
}

fs::path make_target(const fs::path& root, std::string_view id)
{
    return root / std::string(id);
}

// -- copying ----------------------------------------------------------------

/// Removes a folder when it goes out of scope, unless it was kept.
class folder_guard
{
public:
    folder_guard(const file_operations& operations, fs::path path)
        : operations_(operations), path_(std::move(path))
    {
    }
    ~folder_guard()
    {
        if (!path_.empty())
            (void)operations_.remove_all(path_);
    }
    folder_guard(const folder_guard&) = delete;
    folder_guard& operator=(const folder_guard&) = delete;
    void keep() noexcept { path_.clear(); }
    void aim_at(fs::path path) { path_ = std::move(path); }

private:
    const file_operations& operations_;
    fs::path path_;
};

std::atomic<unsigned> name_counter{0};

/// Makes a new empty folder inside `root` whose name starts with `prefix`.
result<fs::path> make_work_folder(const fs::path& root,
                                  std::string_view prefix, std::string* detail)
{
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        std::string name(prefix);
        name += std::to_string(stamp);
        name += '-';
        name += std::to_string(name_counter.fetch_add(1));
        fs::path path = root / name;
        std::error_code ec;
        if (fs::create_directory(path, ec))
            return path;
        if (ec)
        {
            return refuse(errc::io_error, detail,
                          "cannot make a folder in " + shown(root) + ": " +
                              ec.message());
        }
    }
    return refuse(errc::io_error, detail,
                  "cannot find a free name for a work folder in " +
                      shown(root));
}

/// A name inside `root` that nothing uses, starting with `prefix`.  Nothing
/// is made.
fs::path free_name(const fs::path& root, std::string_view prefix)
{
    std::error_code ec;
    for (;;)
    {
        std::string name(prefix);
        name += std::to_string(name_counter.fetch_add(1));
        const fs::path path = root / name;
        if (fs::symlink_status(path, ec).type() == fs::file_type::not_found)
            return path;
    }
}

/// Copies one file from one open handle, taking its digest as it goes, and
/// refuses it when it is not the file that was measured.
result<void> copy_file_once(const fs::path& from, const fs::path& to,
                            const file_entry& expected, std::string* detail)
{
    const std::string name = expected.path;
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(from, ec);
    if (ec || status.type() != fs::file_type::regular)
    {
        return refuse(plugin_errc::layout_invalid, detail,
                      "\"" + name + "\" is not a plain file any more");
    }
    std::ifstream in(from, std::ios::binary);
    if (!in)
    {
        return refuse(errc::io_error, detail,
                      "cannot open \"" + name + "\" to copy it");
    }
    fs::create_directories(to.parent_path(), ec);
    if (ec)
    {
        return refuse(errc::io_error, detail,
                      "cannot make the folder of \"" + name +
                          "\": " + ec.message());
    }
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        return refuse(errc::io_error, detail,
                      "cannot write \"" + name + "\"");
    }

    sha512_hasher hasher;
    std::uint64_t size = 0;
    std::vector<char> buffer(copy_chunk);
    while (in)
    {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in.gcount();
        if (got <= 0)
            break;
        hasher.update(std::string_view(buffer.data(),
                                       static_cast<std::size_t>(got)));
        out.write(buffer.data(), got);
        size += static_cast<std::uint64_t>(got);
        if (size > expected.size)
            break;
    }
    if (in.bad())
    {
        return refuse(errc::io_error, detail,
                      "cannot read \"" + name + "\"");
    }
    out.flush();
    out.close();
    if (out.fail())
    {
        return refuse(errc::io_error, detail,
                      "cannot write \"" + name + "\"");
    }
    if (size != expected.size || hasher.finish() != expected.sha512)
    {
        return refuse(plugin_errc::changed, detail,
                      "\"" + name + "\" changed while it was being copied");
    }
    return {};
}

/// Copies every file of the inspected plugin into `work`, `plugin.json` last,
/// so that a half-made folder is never taken for a plugin, then measures the
/// copy and holds it to the hash of the first look.
result<void> copy_plugin(const inspection& found, const fs::path& work,
                         std::string* detail)
{
    const std::string manifest_name(manifest_file_name);
    for (const file_entry& entry : found.files)
    {
        if (entry.path == manifest_name)
            continue;
        const fs::path relative(entry.path);
        const result<void> done = copy_file_once(found.directory / relative,
                                                 work / relative, entry,
                                                 detail);
        if (done.has_error())
            return done;
    }
    for (const file_entry& entry : found.files)
    {
        if (entry.path != manifest_name)
            continue;
        const result<void> done = copy_file_once(
            found.directory / manifest_name, work / manifest_name, entry,
            detail);
        if (done.has_error())
            return done;
    }

    std::string measure_detail;
    result<std::vector<file_entry>> again =
        measure_directory(work, &measure_detail);
    if (again.has_error())
    {
        set_detail(detail, "the copy cannot be checked: " + measure_detail);
        return again.error();
    }
    if (again.value() != found.files ||
        !(hash_plugin(again.value()) == found.hash))
    {
        return refuse(plugin_errc::changed, detail,
                      "the copy is not the plugin that was checked");
    }
    return {};
}

} // namespace

// -- public -----------------------------------------------------------------

inspection inspect_plugin(const fs::path& directory,
                          const inspect_context& context)
{
    try
    {
        return inspect(directory, context);
    }
    catch (const std::exception&)
    {
        inspection out;
        out.directory = directory;
        out.problem = make_error_code(errc::io_error);
        out.detail = "the plugin folder cannot be looked at";
        return out;
    }
}

bool blocks_install(const inspection& found) noexcept
{
    if (found.problem.failed())
        return true;
    if (found.signature.state == signature_state::invalid)
        return true;
    return found.run.failed() && official_blocks(found.run);
}

error_code install_refusal(const inspection& found, bool allow_unsigned,
                           std::string* detail)
{
    set_detail(detail, std::string{});
    if (blocks_install(found))
    {
        const error_code why = found.problem.failed() ? found.problem
                                                      : found.run;
        const std::string& line =
            found.problem.failed()
                ? found.detail
                : (found.run.failed() ? found.run_detail
                                      : found.signature_detail);
        set_detail(detail, line.empty() ? why.message() : line);
        return why;
    }
    if (found.signature.state == signature_state::absent && !allow_unsigned)
    {
        set_detail(detail, "the plugin is not signed");
        return make_error_code(plugin_errc::signature_missing);
    }
    return {};
}

result<key_list> builtin_key_list(std::string* detail)
{
    return read_key_list(detail::official_key_list_text(), builtin_root_keys(),
                         detail);
}

inspect_context
context_for_this_computer(const computer_options& options,
                          std::vector<std::string>* notes)
{
    inspect_context out;
    const auto note = [notes](std::string text)
    {
        if (notes != nullptr)
            notes->push_back(std::move(text));
    };

    out.today = options.today.value_or(std::chrono::floor<std::chrono::days>(
        std::chrono::system_clock::now()));

    std::string detail;
    result<key_list> list = builtin_key_list(&detail);
    if (list.has_value())
        out.official = std::move(list).value();
    else
        note("the official key list could not be read: " + detail);

    if (!options.program_directory.empty())
    {
        std::string install_detail;
        const result<install_kind> kind =
            kind_of_install(options.program_directory, &install_detail);
        if (kind.has_value())
            out.install = kind.value();
        else
            note("Studio's own folder could not be examined: " +
                 (install_detail.empty() ? kind.error().message()
                                         : install_detail));
    }

    if (!options.policy_file.empty())
    {
        const loaded_policy loaded = load_policy(options.policy_file);
        out.rules = loaded.rules;
        for (const policy_key& key : out.rules ? out.rules->allow_keys
                                               : std::vector<policy_key>{})
        {
            out.administrator_keys.push_back(
                approved_signer{key.key_id, key.public_key});
        }
        if (loaded.problem.failed())
        {
            note("the policy file cannot be used, so only official plugins "
                 "are allowed: " +
                 (loaded.detail.empty() ? loaded.problem.message()
                                        : loaded.detail));
            out.administrator_keys.clear();
        }
    }

    if (!options.trust_file.empty())
    {
        std::string trust_detail;
        result<trust_store> store =
            load_trust_store(options.trust_file, &trust_detail);
        if (store.has_value())
        {
            out.trust = std::move(store).value();
            for (const approved_key& key : out.trust->keys())
            {
                out.user_keys.push_back(
                    approved_signer{key.key_id, key.public_key});
            }
            out.today = out.trust->note_date(out.today);
        }
        else
        {
            note("the trust store cannot be read, so no key is approved: " +
                 (trust_detail.empty() ? store.error().message()
                                       : trust_detail));
        }
    }
    return out;
}

file_operations default_file_operations()
{
    file_operations out;
    out.rename = [](const fs::path& from, const fs::path& to)
    {
        std::error_code ec;
        fs::rename(from, to, ec);
        return from_std(ec);
    };
    out.remove_all = [](const fs::path& path)
    {
        std::error_code ec;
        fs::remove_all(path, ec);
        return from_std(ec);
    };
    return out;
}

bool usable_as_folder_name(std::string_view id) noexcept
{
    if (!detail::valid_plugin_id(id))
        return false;
    const std::string_view first = id.substr(0, id.find('.'));
    return !is_device_name(first);
}

result<install_report> install_plugin(const fs::path& source,
                                      const fs::path& target_root,
                                      const install_options& options,
                                      std::string* detail)
{
    set_detail(detail, std::string{});
    try
    {
        const file_operations real = default_file_operations();
        const file_operations& operations =
            options.operations != nullptr ? *options.operations : real;

        install_report report;
        report.found = inspect_plugin(source, options.context);
        const inspection& found = report.found;

        const error_code refusal =
            install_refusal(found, options.allow_unsigned, detail);
        if (refusal.failed())
            return refusal;
        const manifest& declared = *found.declared;
        if (!usable_as_folder_name(declared.id))
        {
            return refuse(plugin_errc::layout_invalid, detail,
                          "the id of the plugin cannot be the name of a "
                          "folder");
        }

        std::error_code ec;
        fs::create_directories(target_root, ec);
        if (ec)
        {
            return refuse(errc::io_error, detail,
                          "cannot make the plugin folder " +
                              shown(target_root) + ": " + ec.message());
        }
        if (options.for_everyone)
        {
            std::string access_detail;
            const result<write_access> who =
                who_can_write(target_root, &access_detail);
            if (who.has_error())
            {
                return refuse(errc::permission_denied, detail,
                              "cannot tell who may write to " +
                                  shown(target_root) + ": " +
                                  (access_detail.empty()
                                       ? who.error().message()
                                       : access_detail));
            }
            if (who.value() != write_access::administrators_only)
            {
                return refuse(errc::permission_denied, detail,
                              shown(target_root) +
                                  " can be changed by someone who is not an "
                                  "administrator: " +
                                  access_detail);
            }
        }

        report.directory = make_target(target_root, declared.id);
        std::error_code exists_ec;
        const fs::file_status existing =
            fs::symlink_status(report.directory, exists_ec);
        const bool there = !exists_ec &&
                           existing.type() != fs::file_type::not_found;
        if (there && !options.replace)
        {
            return refuse(errc::already_exists, detail,
                          "a plugin with the id " + declared.id +
                              " is already installed in " +
                              shown(target_root));
        }

        std::string work_detail;
        result<fs::path> work =
            make_work_folder(target_root, ".vss-new-", &work_detail);
        if (work.has_error())
        {
            set_detail(detail, work_detail);
            return work.error();
        }
        folder_guard new_folder(operations, work.value());

        std::string copy_detail;
        const result<void> copied =
            copy_plugin(found, work.value(), &copy_detail);
        if (copied.has_error())
        {
            set_detail(detail, copy_detail);
            return copied.error();
        }

        if (!there)
        {
            const error_code moved =
                operations.rename(work.value(), report.directory);
            if (moved.failed())
            {
                return refuse(errc::io_error, detail,
                              "cannot move the plugin into " +
                                  shown(report.directory) + ": " +
                                  moved.message());
            }
            new_folder.keep();
            return report;
        }

        // Replace: the old folder aside, the new one in, the old one gone.
        const fs::path aside = free_name(target_root, ".vss-old-");
        const error_code first = operations.rename(report.directory, aside);
        if (first.failed())
        {
            return refuse(errc::io_error, detail,
                          "cannot move the old copy of " + declared.id +
                              " aside: " + first.message());
        }
        const error_code second =
            operations.rename(work.value(), report.directory);
        if (second.failed())
        {
            const error_code back =
                operations.rename(aside, report.directory);
            std::string text = "cannot move the new copy of " + declared.id +
                               " into place: " + second.message();
            if (back.failed())
            {
                text += "; the old copy could not be put back and is in " +
                        shown(aside) + ": " + back.message();
            }
            else
            {
                text += "; the old copy is as it was";
            }
            return refuse(errc::io_error, detail, std::move(text));
        }
        new_folder.keep();
        report.replaced = true;
        const error_code removed = operations.remove_all(aside);
        if (removed.failed())
        {
            report.warning = "the old copy could not be removed and is in " +
                             shown(aside) + ": " + removed.message();
        }
        return report;
    }
    catch (const std::exception& error)
    {
        return refuse(errc::io_error, detail,
                      std::string("the install failed: ") + error.what());
    }
}

result<void> remove_plugin(const fs::path& target_root,
                           std::string_view plugin_id,
                           const file_operations* operations,
                           std::string* detail)
{
    set_detail(detail, std::string{});
    try
    {
        if (!usable_as_folder_name(plugin_id))
        {
            return refuse(errc::invalid_argument, detail,
                          "that is not the id of a plugin");
        }
        const file_operations real = default_file_operations();
        const file_operations& ops = operations != nullptr ? *operations
                                                           : real;
        const fs::path folder = make_target(target_root, plugin_id);
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(folder, ec);
        if (status.type() == fs::file_type::not_found)
        {
            return refuse(errc::not_found, detail,
                          "no plugin " + std::string(plugin_id) +
                              " is installed in " + shown(target_root));
        }
        if (ec)
        {
            return refuse(errc::io_error, detail,
                          "cannot look at " + shown(folder) + ": " +
                              ec.message());
        }
        if (status.type() != fs::file_type::directory)
        {
            return refuse(plugin_errc::layout_invalid, detail,
                          shown(folder) + " is not a plugin folder");
        }
        const std::string manifest_name(manifest_file_name);
        const fs::file_status file =
            fs::symlink_status(folder / manifest_name, ec);
        if (ec || file.type() != fs::file_type::regular)
        {
            return refuse(plugin_errc::layout_invalid, detail,
                          shown(folder) + " has no " + manifest_name +
                              ", so it is not a plugin folder");
        }
        const error_code removed = ops.remove_all(folder);
        if (removed.failed())
        {
            return refuse(errc::io_error, detail,
                          "cannot remove " + shown(folder) + ": " +
                              removed.message());
        }
        return {};
    }
    catch (const std::exception& error)
    {
        return refuse(errc::io_error, detail,
                      std::string("the removal failed: ") + error.what());
    }
}

} // namespace vss::plugin
