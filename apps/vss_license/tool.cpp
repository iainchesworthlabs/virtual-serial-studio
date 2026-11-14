//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "tool.hpp"

#include "platform.hpp"

#include <vss/license/sign.hpp>
#include <vss/license/signed_file.hpp>
#include <vss/license/test_keys.hpp>

#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <ostream>
#include <set>
#include <string_view>
#include <vector>

namespace vss::license::tool
{

namespace
{

namespace sf = signed_file;

constexpr std::string_view usage_text =
    "vss_license - reads, checks and issues Virtual Serial Studio licence "
    "files.\n"
    "It works offline and never prints a private key.\n"
    "\n"
    "  vss_license inspect <file> [--test]\n"
    "      Prints what a licence or revocation file says and whether its\n"
    "      signature verifies.\n"
    "\n"
    "  vss_license verify <file> [--at <date>] [--machine <name>]\n"
    "                     [--revocations <file>] [--test]\n"
    "      Runs the check Studio runs. Exit status 0 when the licence's\n"
    "      features work (valid, expiring or grace), 1 otherwise.\n"
    "      <date> is 2027-03-01 or 2027-03-01T00:00:00Z.\n"
    "\n"
    "  vss_license issue --request <file> --out <file> --key-id <id>\n"
    "                    (--key-file <path> | --key-env <name>)\n"
    "                    [--ledger <file>]\n"
    "  vss_license issue --request <file> --dry-run [--ledger <file>]\n"
    "  vss_license issue --request <file> --out <file> --test\n"
    "      Checks a request (the payload fields), signs it, checks its own\n"
    "      output and writes the licence. --dry-run checks and prints only\n"
    "      and uses no key. --test signs with the published test key; no\n"
    "      product accepts such a licence.\n"
    "\n"
    "  vss_license revocations --request <file> --out <file> ...\n"
    "      Signs a revocation list. The key options are those of issue.\n"
    "\n"
    "  vss_license keygen --key-id <id> --out-private <path>\n"
    "      Makes a key pair, writes the private half to a new file that only\n"
    "      you can read, and prints the public half.\n"
    "\n"
    "The private key is read from a file or an environment variable, never\n"
    "from the command line.\n";

/// The words of a command line after the command name.
struct arguments
{
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;
    std::set<std::string> flags;

    const std::string* option(const std::string& name) const
    {
        const auto found = options.find(name);
        return found == options.end() ? nullptr : &found->second;
    }
    bool flag(const std::string& name) const { return flags.contains(name); }
};

/// Sorts the words into options that take a value, flags and the rest.
/// Empty when a word is not one the command knows, or a value is missing or
/// given twice.
std::optional<arguments> parse_arguments(std::span<const std::string> words,
                                         const std::set<std::string>& options,
                                         const std::set<std::string>& flags,
                                         std::ostream& err)
{
    arguments parsed;
    for (std::size_t index = 0; index < words.size(); ++index)
    {
        const std::string& word = words[index];
        if (!word.starts_with("--"))
        {
            parsed.positional.push_back(word);
        }
        else if (flags.contains(word))
        {
            parsed.flags.insert(word);
        }
        else if (options.contains(word))
        {
            if (index + 1 >= words.size())
            {
                err << word << " needs a value.\n";
                return std::nullopt;
            }
            if (!parsed.options.emplace(word, words[++index]).second)
            {
                err << word << " is given twice.\n";
                return std::nullopt;
            }
        }
        else
        {
            err << "Unknown option " << word << ".\n";
            return std::nullopt;
        }
    }
    return parsed;
}

/// A file's bytes, or nothing when it cannot be read or is larger than a
/// signed file may be.
std::optional<std::string> read_file(const std::string& path)
{
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file)
        return std::nullopt;
    std::string text(sf::max_file_size + 1, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    const auto count = static_cast<std::size_t>(file.gcount());
    if (count > sf::max_file_size)
        return std::nullopt;
    text.resize(count);
    return text;
}

std::span<const std::uint8_t> bytes_of(std::string_view text)
{
    // Reading characters as bytes.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

std::vector<sf::trusted_key> keys_for(bool with_test_keys)
{
    std::vector<sf::trusted_key> keys = sf::product_trust_list();
    if (with_test_keys)
    {
        const auto test = test_keys::trusted();
        keys.insert(keys.end(), test.begin(), test.end());
    }
    return keys;
}

std::string_view name_of(state value)
{
    switch (value)
    {
    case state::none:
        return "none";
    case state::valid:
        return "valid";
    case state::expiring:
        return "expiring";
    case state::grace:
        return "grace";
    case state::expired:
        return "expired";
    case state::not_yet_valid:
        return "not yet valid";
    case state::clock_suspect:
        return "clock suspect";
    case state::invalid:
        return "invalid";
    }
    return "unknown";
}

std::string_view sentence_for(reason value)
{
    switch (value)
    {
    case reason::ok:
        return "ok";
    case reason::no_licence:
        return "there is no licence";
    case reason::expired:
        return "the licence has ended";
    case reason::not_yet_valid:
        return "the licence has not started yet";
    case reason::clock_suspect:
        return "the date is before a time that has certainly passed";
    case reason::malformed:
        return "the file is damaged or is not a licence";
    case reason::newer_version:
        return "the licence needs a newer version";
    case reason::bad_signature:
        return "the signature does not verify";
    case reason::revoked:
        return "the licence or its key was withdrawn";
    case reason::wrong_computer:
        return "the licence does not name this computer";
    case reason::not_in_licence:
        return "the licence does not include the feature";
    }
    return "unknown";
}

std::string_view name_of(licence_kind value)
{
    switch (value)
    {
    case licence_kind::trial:
        return "trial";
    case licence_kind::annual:
        return "annual";
    case licence_kind::enterprise:
        return "enterprise";
    }
    return "unknown";
}

std::string_view name_of(edition value)
{
    return value == edition::enterprise ? "enterprise" : "pro";
}

std::string joined(const std::vector<std::string>& values)
{
    std::string text;
    for (const std::string& value : values)
    {
        if (!text.empty())
            text += ", ";
        text += value;
    }
    return text;
}

void print_licence(std::ostream& out, const licence& value)
{
    out << "Licence: " << value.licence_id << '\n'
        << "Kind: " << name_of(value.kind) << '\n'
        << "Edition: " << name_of(value.edition) << '\n'
        << "Licensee: " << value.licensee_name << '\n';
    if (!value.licensee_contact.empty())
        out << "Contact: " << value.licensee_contact << '\n';
    out << "Issued: " << format_time(value.issued_at) << '\n'
        << "Starts: " << format_time(value.starts_at) << '\n'
        << "Expires: " << format_time(value.expires_at) << '\n'
        << "Grace days: " << value.grace_days << '\n';
    if (value.seats == 0)
        out << "Seats: not counted by seat\n";
    else
        out << "Seats: " << value.seats << '\n';
    out << "Features: " << joined(value.features) << '\n';
    if (!value.machines.empty())
        out << "Computers: " << joined(value.machines) << '\n';
    if (value.updates_until)
        out << "Updates until: " << format_time(*value.updates_until) << '\n';
    if (!value.replaces.empty())
        out << "Replaces: " << value.replaces << '\n';
    if (!value.clock_reset.empty())
        out << "Clock reset: yes\n";
}

void print_revocations(std::ostream& out, const revocation_list& value)
{
    out << "Sequence: " << value.sequence << '\n'
        << "Issued: " << format_time(value.issued_at) << '\n'
        << "Licences withdrawn: " << value.licence_ids.size() << '\n';
    for (const std::string& id : value.licence_ids)
        out << "  " << id << '\n';
    out << "Keys withdrawn: " << value.keys.size() << '\n';
    for (const revoked_key& key : value.keys)
    {
        out << "  " << key.key_id;
        if (key.from)
            out << " from " << format_time(*key.from);
        out << '\n';
    }
}

/// `2027-03-01` or `2027-03-01T00:00:00Z`.
std::optional<time_point> parse_date(const std::string& text)
{
    const auto full =
        parse_time(text.size() == 10 ? text + "T00:00:00Z" : text);
    if (!full)
        return std::nullopt;
    return *full;
}

int inspect(std::span<const std::string> words, surroundings& where)
{
    const auto args = parse_arguments(words, {}, {"--test"}, where.err);
    if (!args || args->positional.size() != 1)
        return exit_usage;

    const auto text = read_file(args->positional.front());
    if (!text)
    {
        where.err << "Cannot read " << args->positional.front()
                  << ", or it is too large to be a signed file.\n";
        return exit_failed;
    }
    const auto file = sf::parse(*text);
    if (!file)
    {
        where.err << "Not a signed file: " << file.error().message() << ".\n";
        return exit_failed;
    }

    where.out << "Format: " << file->format << '\n'
              << "Purpose: " << file->purpose << '\n'
              << "Key: " << file->key_id << '\n';

    const auto keys = keys_for(args->flag("--test"));
    const auto key = sf::verify(*file, file->purpose, keys);
    if (key)
        where.out << "Signature: verifies\n";
    else
        where.out << "Signature: NOT VERIFIED (" << key.error().message()
                  << "). Nothing below can be trusted.\n";

    if (file->purpose == licence_purpose)
    {
        const auto value = parse_payload(file->payload);
        if (value)
            print_licence(where.out, *value);
        else
            where.out << "Payload: cannot be read (" << value.error().message()
                      << ")\n";
    }
    else if (file->purpose == revocations_purpose)
    {
        const auto value = parse_revocations(file->payload);
        if (value)
            print_revocations(where.out, *value);
        else
            where.out << "Payload: cannot be read (" << value.error().message()
                      << ")\n";
    }
    else
    {
        where.out << "Payload: " << file->payload.size()
                  << " bytes, not a kind this tool reads\n";
    }
    return exit_ok;
}

int verify(std::span<const std::string> words, surroundings& where)
{
    const auto args = parse_arguments(
        words, {"--at", "--machine", "--revocations"}, {"--test"}, where.err);
    if (!args || args->positional.size() != 1)
        return exit_usage;

    const auto keys = keys_for(args->flag("--test"));

    context judged;
    judged.now = where.now;
    judged.keys = keys;
    if (const std::string* at = args->option("--at"))
    {
        const auto when = parse_date(*at);
        if (!when)
        {
            where.err << "--at needs a date such as 2027-03-01.\n";
            return exit_usage;
        }
        judged.now = *when;
    }
    if (const std::string* machine = args->option("--machine"))
        judged.computer_name = *machine;

    revocation_list revoked;
    if (const std::string* path = args->option("--revocations"))
    {
        const auto text = read_file(*path);
        if (!text)
        {
            where.err << "Cannot read " << *path << ".\n";
            return exit_failed;
        }
        auto list = read_revocations(*text, keys);
        if (!list)
        {
            where.err << "The revocation list is not valid: "
                      << list.error().message() << ".\n";
            return exit_failed;
        }
        revoked = std::move(*list);
        judged.revocations = &revoked;
    }

    const auto text = read_file(args->positional.front());
    if (!text)
    {
        where.err << "Cannot read " << args->positional.front()
                  << ", or it is too large to be a licence.\n";
        return exit_failed;
    }

    const status result = evaluate(*text, judged);
    where.out << "State: " << name_of(result.state) << '\n'
              << "Reason: " << sentence_for(result.reason) << '\n'
              << "Judged at: " << format_time(judged.now) << '\n';
    if (result.value)
    {
        where.out << "Licence: " << result.value->licence_id << '\n'
                  << "Key: " << result.key_id << '\n';
        if (result.active())
            where.out << "Days left: " << result.days_left << '\n';
    }
    return result.active() ? exit_ok : exit_failed;
}

/// The licence ids a ledger holds. The ledger is the owner's audit log: one
/// JSON object on each line, with a `licence_id`. Empty optional when the
/// file cannot be read or a line is not such an object.
std::optional<std::set<std::string>> ledger_ids(const std::string& path)
{
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file)
        return std::nullopt;
    std::set<std::string> ids;
    std::string line;
    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        boost::system::error_code failed;
        const boost::json::value value = boost::json::parse(line, failed);
        if (failed || !value.is_object())
            return std::nullopt;
        const boost::json::value* id =
            value.as_object().if_contains("licence_id");
        if (id != nullptr && id->is_string())
            ids.emplace(id->as_string());
    }
    return ids;
}

bool key_id_is_well_formed(std::string_view id)
{
    constexpr std::size_t longest = 64;
    return !id.empty() && id.size() <= longest &&
           std::ranges::all_of(id,
                               [](char c)
                               {
                                   return (c >= 'a' && c <= 'z') ||
                                          (c >= '0' && c <= '9') || c == '-';
                               });
}

/// The key a command signs with.
struct signing_key
{
    std::string key_id;
    signing::seed seed{};
    bool test = false;

    signing_key() = default;
    signing_key(const signing_key&) = delete;
    signing_key& operator=(const signing_key&) = delete;
    ~signing_key() { signing::wipe(seed); }
};

/// Reads the key the options name into `key`. Returns an exit status, or
/// nothing when the key is ready.
std::optional<int> load_key(const arguments& args, surroundings& where,
                            signing_key& key)
{
    const std::string* key_file = args.option("--key-file");
    const std::string* key_env = args.option("--key-env");
    const std::string* key_id = args.option("--key-id");
    const bool test = args.flag("--test");

    if (test)
    {
        if (key_file != nullptr || key_env != nullptr || key_id != nullptr)
        {
            where.err << "--test uses the published test key; it takes no "
                         "--key-file, --key-env or --key-id.\n";
            return exit_usage;
        }
        key.key_id = std::string(test_keys::key_id);
        key.seed = test_keys::private_key();
        key.test = true;
        return std::nullopt;
    }

    if ((key_file == nullptr) == (key_env == nullptr))
    {
        where.err << "Give the key with --key-file or with --key-env, one "
                     "of the two.\n";
        return exit_usage;
    }
    if (key_id == nullptr)
    {
        where.err << "--key-id is needed.\n";
        return exit_usage;
    }
    if (!key_id_is_well_formed(*key_id))
    {
        where.err << "A key id is lower-case letters, digits and hyphens.\n";
        return exit_usage;
    }
    if (key_id->starts_with(sf::test_key_prefix))
    {
        where.err << "A key id that starts with test- is a test key. Use "
                     "--test.\n";
        return exit_usage;
    }

    std::optional<std::string> text;
    if (key_file != nullptr)
        text = read_file(*key_file);
    else if (where.variable)
        text = where.variable(*key_env);
    if (!text)
    {
        where.err << "Cannot read the private key.\n";
        return exit_failed;
    }
    auto seed = signing::parse_seed(*text);
    signing::wipe(*text);
    if (!seed)
    {
        // Says nothing about the text itself.
        where.err << "The private key is not in the form keygen writes.\n";
        return exit_failed;
    }
    key.key_id = *key_id;
    key.seed = *seed;
    signing::wipe(*seed);
    return std::nullopt;
}

bool write_new_file(const std::string& path, std::string_view text)
{
    const std::filesystem::path target(path);
    std::error_code ignored;
    if (std::filesystem::exists(target, ignored))
        return false;
    std::ofstream file(target, std::ios::binary);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.flush();
    return file.good();
}

std::set<std::string> signing_options()
{
    return {"--request",  "--out",     "--key-id",
            "--key-file", "--key-env", "--ledger"};
}

std::set<std::string> signing_flags()
{
    return {"--test", "--dry-run"};
}

/// Signs `payload`, checks the result as a reader would and writes it.
int sign_and_write(const arguments& args, surroundings& where,
                   std::string_view purpose, const std::string& payload,
                   time_point issued_at)
{
    const std::string* out = args.option("--out");
    if (out == nullptr)
    {
        where.err << "--out is needed.\n";
        return exit_usage;
    }
    std::error_code ignored;
    if (std::filesystem::exists(std::filesystem::path(*out), ignored))
    {
        where.err << *out << " exists already. It is not overwritten.\n";
        return exit_failed;
    }

    signing_key key;
    if (const auto status = load_key(args, where, key))
        return *status;

    const sf::envelope file =
        signing::sign(purpose, key.key_id, bytes_of(payload), key.seed);
    const std::string text = sf::write(file);

    // The check a reader makes, on the text that is about to be written.
    const auto keys = keys_for(key.test);
    const auto read_back = sf::parse(text);
    if (!read_back)
    {
        where.err << "The signed file does not read back: "
                  << read_back.error().message() << ".\n";
        return exit_failed;
    }
    const auto trusted = sf::verify(*read_back, purpose, keys);
    if (!trusted)
    {
        where.err << "The signed file does not verify with this build: "
                  << trusted.error().message()
                  << ".\nThe public half of the key must be in the trust "
                     "list of the build that issues.\n";
        return exit_failed;
    }
    if (const auto window = sf::check_key_window(**trusted, issued_at); !window)
    {
        where.err << "The key may not sign a file issued at "
                  << format_time(issued_at) << ": " << window.error().message()
                  << ".\n";
        return exit_failed;
    }

    if (!write_new_file(*out, text))
    {
        where.err << "Cannot write " << *out << ".\n";
        return exit_failed;
    }
    where.out << "Wrote " << *out << '\n' << "Key: " << key.key_id << '\n';
    if (key.test)
        where.out << "Signed with the test key. No product accepts this "
                     "file.\n";
    return exit_ok;
}

/// The request file of a signing command, or an exit status.
std::optional<std::string> read_request(const arguments& args,
                                        surroundings& where, int& status)
{
    const std::string* request = args.option("--request");
    if (request == nullptr || !args.positional.empty())
    {
        status = exit_usage;
        return std::nullopt;
    }
    auto text = read_file(*request);
    if (!text)
    {
        where.err << "Cannot read " << *request << ".\n";
        status = exit_failed;
    }
    return text;
}

int issue(std::span<const std::string> words, surroundings& where)
{
    const auto args =
        parse_arguments(words, signing_options(), signing_flags(), where.err);
    if (!args)
        return exit_usage;
    int status = exit_ok;
    const auto request = read_request(*args, where, status);
    if (!request)
        return status;

    const auto value = parse_payload(bytes_of(*request));
    if (!value)
    {
        where.err << "The request is not a licence payload: "
                  << value.error().message() << ".\n";
        return exit_failed;
    }
    const auto payload = write_payload(*value);
    if (!payload)
    {
        where.err << "The request cannot be written as a licence: "
                  << payload.error().message() << ".\n";
        return exit_failed;
    }

    if (const std::string* ledger = args->option("--ledger"))
    {
        const auto ids = ledger_ids(*ledger);
        if (!ids)
        {
            where.err << "Cannot read the ledger " << *ledger << ".\n";
            return exit_failed;
        }
        if (ids->contains(value->licence_id))
        {
            where.err << value->licence_id
                      << " is in the ledger already. A licence id is never "
                         "used twice.\n";
            return exit_failed;
        }
    }

    print_licence(where.out, *value);
    if (args->flag("--dry-run"))
    {
        where.out << "Dry run: nothing was signed or written.\n";
        return exit_ok;
    }
    return sign_and_write(*args, where, licence_purpose, *payload,
                          value->issued_at);
}

int revocations(std::span<const std::string> words, surroundings& where)
{
    const auto args =
        parse_arguments(words, signing_options(), signing_flags(), where.err);
    if (!args)
        return exit_usage;
    if (args->option("--ledger") != nullptr)
    {
        where.err << "--ledger is for issue.\n";
        return exit_usage;
    }
    int status = exit_ok;
    const auto request = read_request(*args, where, status);
    if (!request)
        return status;

    const auto value = parse_revocations(bytes_of(*request));
    if (!value)
    {
        where.err << "The request is not a revocation list: "
                  << value.error().message() << ".\n";
        return exit_failed;
    }
    const auto payload = write_revocations(*value);
    if (!payload)
    {
        where.err << "The request cannot be written as a revocation list: "
                  << payload.error().message() << ".\n";
        return exit_failed;
    }

    print_revocations(where.out, *value);
    if (args->flag("--dry-run"))
    {
        where.out << "Dry run: nothing was signed or written.\n";
        return exit_ok;
    }
    return sign_and_write(*args, where, revocations_purpose, *payload,
                          value->issued_at);
}

int keygen(std::span<const std::string> words, surroundings& where)
{
    const auto args =
        parse_arguments(words, {"--key-id", "--out-private"}, {}, where.err);
    if (!args || !args->positional.empty())
        return exit_usage;
    const std::string* key_id = args->option("--key-id");
    const std::string* out = args->option("--out-private");
    if (key_id == nullptr || out == nullptr)
        return exit_usage;
    if (!key_id_is_well_formed(*key_id))
    {
        where.err << "A key id is lower-case letters, digits and hyphens.\n";
        return exit_usage;
    }
    if (key_id->starts_with(sf::test_key_prefix))
    {
        where.err << "A key id that starts with test- is refused by every "
                     "product. Choose another.\n";
        return exit_usage;
    }

    signing_key key;
    if (!platform::random_bytes(key.seed))
    {
        where.err << "The system gave no random numbers. No key was made.\n";
        return exit_failed;
    }
    const sf::public_key public_half = signing::public_key_of(key.seed);

    std::string text = signing::format_seed(key.seed);
    text += '\n';
    const bool written = platform::write_private_file(*out, text);
    signing::wipe(text);
    if (!written)
    {
        where.err << "Cannot make " << *out << ". It must not exist already.\n";
        return exit_failed;
    }

    where.out << "Wrote the private key to " << *out
              << ". Only you can read that file. Keep it off shared disks "
                 "and out of any repository.\n"
              << "Key id: " << *key_id << '\n'
              << "Public key: " << sf::base64url_encode(public_half) << '\n'
              << "Public key bytes, for the trust list:\n   ";
    constexpr std::string_view digits = "0123456789abcdef";
    for (std::size_t index = 0; index < public_half.size(); ++index)
    {
        const unsigned byte = public_half[index];
        where.out << " 0x" << digits[byte >> 4U] << digits[byte & 0x0fU]
                  << (index + 1 < public_half.size() ? "," : "");
        if (index % 8 == 7 && index + 1 < public_half.size())
            where.out << "\n   ";
    }
    where.out << '\n';
    return exit_ok;
}

} // namespace

std::string_view usage()
{
    return usage_text;
}

int run(std::span<const std::string> arguments, surroundings& where)
{
    if (arguments.empty() || arguments.front() == "--help" ||
        arguments.front() == "help")
    {
        where.out << usage_text;
        return arguments.empty() ? exit_usage : exit_ok;
    }

    const std::string& command = arguments.front();
    const auto rest = arguments.subspan(1);
    int status = exit_usage;
    if (command == "inspect")
        status = inspect(rest, where);
    else if (command == "verify")
        status = verify(rest, where);
    else if (command == "issue")
        status = issue(rest, where);
    else if (command == "revocations")
        status = revocations(rest, where);
    else if (command == "keygen")
        status = keygen(rest, where);
    else
        where.err << "Unknown command " << command << ".\n";

    if (status == exit_usage)
        where.err << "Run vss_license --help for the commands.\n";
    return status;
}

} // namespace vss::license::tool
