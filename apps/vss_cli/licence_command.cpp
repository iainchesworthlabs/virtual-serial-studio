//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// vss_cli licence - the licence Studio uses on this computer.
//
//   (no word)     what Studio sees: the edition, the licence and the files
//   import <file> check a licence file and copy it to the user's folder
//
// It reads files and prints. It uses no network, and it never prints a key or
// the contents of a file. Text that came from a file or a folder is printed
// with its control characters replaced, so that it cannot move the cursor or
// change the colours of the terminal.
//

#include "licence_command.hpp"

#include <vss/license/license.hpp>
#include <vss/license/store.hpp>
#include <vss/license/words.hpp>

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <ostream>
#include <string>
#include <string_view>

namespace vss_cli
{
namespace
{

namespace lic = vss::license;

constexpr int exit_ok = 0;
constexpr int exit_failed = 1;
constexpr int exit_usage = 2;

/// `text` with every control character replaced by `?`. Bytes of a UTF-8
/// character are kept.
std::string plain(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text)
    {
        const auto byte = static_cast<unsigned char>(c);
        out += byte < 0x20 || byte == 0x7f ? '?' : c;
    }
    return out;
}

/// `trial`, `annual` or `enterprise`: the kind of licence a file is.
std::string_view kind_name(lic::licence_kind kind)
{
    switch (kind)
    {
    case lic::licence_kind::trial:
        return "trial";
    case lic::licence_kind::annual:
        return "annual";
    case lic::licence_kind::enterprise:
        return "enterprise";
    }
    return "annual";
}

/// "2027-03-01", the date of a time in UTC.
std::string date_of(lic::time_point value)
{
    return lic::format_time(value).substr(0, 10);
}

void print_view(std::ostream& out, const lic::store_view& view)
{
    const lic::status& status = view.status;
    // The edition and the state are the words the control gateway's status
    // uses, so a script reads the same value from either.
    out << "Edition: " << lic::edition_name(status) << '\n'
        << "State: " << lic::state_name(status.state) << '\n';
    if (status.reason != lic::reason::ok)
        out << "Reason: " << lic::reason_sentence(status.reason) << '\n';
    if (status.value)
    {
        const lic::licence& value = *status.value;
        out << "Licence: " << plain(value.licence_id) << '\n'
            << "Kind: " << kind_name(value.kind) << '\n'
            << "Licensed to: " << plain(value.licensee_name) << '\n'
            << "Term: " << date_of(value.starts_at) << " to "
            << date_of(value.expires_at) << '\n';
        if (status.active())
            out << "Days left: " << status.days_left << '\n';
        if (value.seats == 0)
            out << "Seats: site licence\n";
        else
            out << "Seats: " << value.seats << '\n';
    }
    if (view.files.empty())
    {
        out << "Files: none\n";
        return;
    }
    out << "Files:\n";
    for (const lic::found_licence& file : view.files)
    {
        out << "  " << plain(file.file_name) << " ("
            << (file.machine_wide ? "machine" : "user") << ", "
            << lic::state_name(file.status.state)
            << (file.in_use ? ", in use" : "") << ")\n";
    }
}

int command_show(const lic::store_options& options, std::ostream& out)
{
    lic::store store(options);
    store.refresh();
    print_view(out, store.view());
    return exit_ok;
}

/// The one sentence for an import.
int command_import(const std::string& file, const lic::store_options& options,
                   std::ostream& out, std::ostream& err)
{
    lic::store store(options);
    const lic::import_result result =
        store.import_file(std::filesystem::path(file));
    switch (result.outcome)
    {
    case lic::import_outcome::imported:
    {
        out << "Imported licence ";
        if (result.status.value)
            out << plain(result.status.value->licence_id) << ' ';
        out << "to the licence folder; its state is "
            << lic::state_word(result.status.state) << ".\n";
        return exit_ok;
    }
    case lic::import_outcome::already_installed:
        out << "This licence is installed already; nothing was copied.\n";
        return exit_ok;
    case lic::import_outcome::not_valid:
        err << "Not imported: " << lic::reason_sentence(result.status.reason)
            << ".\n";
        return exit_failed;
    case lic::import_outcome::unreadable:
        break;
    }
    err << "Not imported: the file cannot be read, or the licence folder "
           "cannot be written.\n";
    return exit_failed;
}

} // namespace

const char* licence_usage() noexcept
{
    return "  vss_cli licence\n"
           "      Show the edition Studio uses on this computer, the licence "
           "behind\n"
           "      it and the licence files found in the licence folders.  "
           "Reads\n"
           "      files only.  Exit code 0 means it could look, whatever it "
           "found.\n"
           "\n"
           "  vss_cli licence import <file>\n"
           "      Check a licence file and copy it into your licence folder.  "
           "A\n"
           "      file that is not valid is not copied, and the original is "
           "left\n"
           "      where it was.  Exit code 0 means the licence is installed, "
           "1\n"
           "      that nothing was copied.\n"
           "\n";
}

int command_licence(const std::vector<std::string>& arguments)
{
    return command_licence(arguments, lic::default_store_options(), std::cout,
                           std::cerr);
}

int command_licence(const std::vector<std::string>& arguments,
                    const lic::store_options& options, std::ostream& out,
                    std::ostream& err)
{
    int status = exit_usage;
    if (arguments.size() == 1)
    {
        status = command_show(options, out);
    }
    else if (arguments.size() == 3 && arguments[1] == "import")
    {
        status = command_import(arguments[2], options, out, err);
    }
    else
    {
        err << "licence: unknown command or wrong arguments.  The commands "
               "are:\n"
            << licence_usage();
    }
    out.flush();
    err.flush();
    return status;
}

} // namespace vss_cli
