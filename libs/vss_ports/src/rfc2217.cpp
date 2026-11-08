//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/ports/rfc2217.hpp>

#include <vss/core/profile.hpp>

#include <cstdint>

namespace vss::rfc2217
{
namespace
{

constexpr unsigned char iac = 255;
constexpr unsigned char dont = 254;
constexpr unsigned char iac_do = 253;
constexpr unsigned char wont = 252;
constexpr unsigned char will = 251;
constexpr unsigned char sb = 250;
constexpr unsigned char se = 240;

constexpr unsigned char option_binary = 0;
constexpr unsigned char option_suppress_go_ahead = 3;
constexpr unsigned char option_com_port = 44;

// Client to server command codes. The server answers with the code plus 100.
constexpr unsigned char cmd_signature = 0;
constexpr unsigned char cmd_set_baudrate = 1;
constexpr unsigned char cmd_set_datasize = 2;
constexpr unsigned char cmd_set_parity = 3;
constexpr unsigned char cmd_set_stopsize = 4;
constexpr unsigned char cmd_set_control = 5;
constexpr unsigned char cmd_notify_modemstate = 7;
constexpr unsigned char cmd_set_linestate_mask = 10;
constexpr unsigned char cmd_set_modemstate_mask = 11;
constexpr unsigned char cmd_purge_data = 12;
constexpr unsigned char server_offset = 100;

constexpr unsigned max_baud_rate = 40'000'000;
constexpr std::size_t max_subnegotiation = 256;

constexpr const char* signature = "Virtual Serial Studio";

void put_command(std::string& out, unsigned char verb, unsigned char code)
{
    out.push_back(static_cast<char>(iac));
    out.push_back(static_cast<char>(verb));
    out.push_back(static_cast<char>(code));
}

void put_subnegotiation(std::string& out, unsigned char code,
                        const std::string& value)
{
    out.push_back(static_cast<char>(iac));
    out.push_back(static_cast<char>(sb));
    out.push_back(static_cast<char>(option_com_port));
    out.push_back(static_cast<char>(code + server_offset));
    for (const char byte : value)
    {
        out.push_back(byte);
        if (static_cast<unsigned char>(byte) == iac)
            out.push_back(byte);
    }
    out.push_back(static_cast<char>(iac));
    out.push_back(static_cast<char>(se));
}

std::string one_byte(unsigned value)
{
    return std::string(1, static_cast<char>(value & 0xFFU));
}

std::string baud_bytes(unsigned value)
{
    std::string out;
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<char>((value >> shift) & 0xFFU));
    return out;
}

unsigned parity_code(parity_mode parity)
{
    switch (parity)
    {
    case parity_mode::none:
        return 1;
    case parity_mode::odd:
        return 2;
    case parity_mode::even:
        return 3;
    case parity_mode::mark:
        return 4;
    case parity_mode::space:
        return 5;
    }
    return 1;
}

unsigned stop_code(stop_bits_mode stop)
{
    switch (stop)
    {
    case stop_bits_mode::one:
        return 1;
    case stop_bits_mode::two:
        return 2;
    case stop_bits_mode::one_point_five:
        return 3;
    }
    return 1;
}

unsigned flow_code(flow_control_mode flow)
{
    switch (flow)
    {
    case flow_control_mode::none:
        return 1;
    case flow_control_mode::software:
        return 2;
    case flow_control_mode::hardware:
        return 3;
    }
    return 1;
}

} // namespace

session::session()
{
    settings_.baud_rate = 9600;
}

std::string session::greeting()
{
    // Each option offered is marked as asked, so the client's answer to it is
    // not answered again.
    for (option* entry : {&com_port_, &binary_, &suppress_go_ahead_})
    {
        entry->local = option_state::asked;
        if (entry != &com_port_)
            entry->remote = option_state::asked;
    }

    std::string out;
    put_command(out, will, option_com_port);
    put_command(out, will, option_binary);
    put_command(out, iac_do, option_binary);
    put_command(out, will, option_suppress_go_ahead);
    put_command(out, iac_do, option_suppress_go_ahead);
    return out;
}

bool session::com_port_active() const noexcept
{
    return com_port_.local == option_state::on ||
           com_port_.remote == option_state::on;
}

std::string session::encode(std::string_view data)
{
    std::string out;
    out.reserve(data.size());
    for (const char byte : data)
    {
        out.push_back(byte);
        if (static_cast<unsigned char>(byte) == iac)
            out.push_back(byte);
    }
    return out;
}

session::option& session::option_for(unsigned char code)
{
    switch (code)
    {
    case option_binary:
        return binary_;
    case option_suppress_go_ahead:
        return suppress_go_ahead_;
    case option_com_port:
        return com_port_;
    default:
        break;
    }
    return unsupported_;
}

feed_result session::feed(std::string_view bytes)
{
    VSS_PROFILE_ZONE("ports.rfc2217.feed");
    feed_result result;
    for (const char raw : bytes)
    {
        const auto byte = static_cast<unsigned char>(raw);
        switch (state_)
        {
        case parse_state::data:
            if (byte == iac)
                state_ = parse_state::iac;
            else
                result.data.push_back(raw);
            break;
        case parse_state::iac:
            if (byte == iac)
            {
                result.data.push_back(raw);
                state_ = parse_state::data;
            }
            else if (byte == will || byte == wont || byte == iac_do ||
                     byte == dont)
            {
                verb_ = byte;
                state_ = parse_state::verb;
            }
            else if (byte == sb)
            {
                subnegotiation_.clear();
                state_ = parse_state::subnegotiation;
            }
            else
            {
                state_ = parse_state::data; // NOP, AYT and the like
            }
            break;
        case parse_state::verb:
            negotiate(verb_, byte, result);
            state_ = parse_state::data;
            break;
        case parse_state::subnegotiation:
            if (byte == iac)
                state_ = parse_state::subnegotiation_iac;
            else if (subnegotiation_.size() < max_subnegotiation)
                subnegotiation_.push_back(raw);
            break;
        case parse_state::subnegotiation_iac:
            if (byte == iac)
            {
                if (subnegotiation_.size() < max_subnegotiation)
                    subnegotiation_.push_back(raw);
                state_ = parse_state::subnegotiation;
            }
            else
            {
                if (byte == se)
                    subnegotiate(subnegotiation_, result);
                state_ = parse_state::data;
            }
            break;
        }
    }
    return result;
}

void session::negotiate(unsigned char verb, unsigned char code,
                        feed_result& result)
{
    const bool supported = code == option_binary ||
                           code == option_suppress_go_ahead ||
                           code == option_com_port;
    option& entry = option_for(code);

    switch (verb)
    {
    case iac_do:
        if (!supported)
        {
            put_command(result.reply, wont, code);
            break;
        }
        if (entry.local == option_state::off)
            put_command(result.reply, will, code);
        entry.local = option_state::on; // "asked" is answered by this DO
        break;
    case dont:
        if (supported && entry.local == option_state::on)
            put_command(result.reply, wont, code);
        entry.local = option_state::off;
        break;
    case will:
        if (!supported)
        {
            put_command(result.reply, dont, code);
            break;
        }
        if (entry.remote == option_state::off)
            put_command(result.reply, iac_do, code);
        entry.remote = option_state::on; // "asked" is answered by this WILL
        break;
    case wont:
        if (supported && entry.remote == option_state::on)
            put_command(result.reply, dont, code);
        entry.remote = option_state::off;
        break;
    default:
        break;
    }
}

void session::subnegotiate(const std::string& payload, feed_result& result)
{
    if (payload.size() < 2 ||
        static_cast<unsigned char>(payload[0]) != option_com_port)
        return;
    command(static_cast<unsigned char>(payload[1]), payload.substr(2), result);
}

unsigned session::set_control(unsigned request, feed_result& result)
{
    unsigned answer = request;
    switch (request)
    {
    case 0:
        answer = flow_code(settings_.flow_control);
        break;
    case 1:
        settings_.flow_control = flow_control_mode::none;
        break;
    case 2:
        settings_.flow_control = flow_control_mode::software;
        break;
    case 3:
        settings_.flow_control = flow_control_mode::hardware;
        break;
    case 4:
        answer = outputs_.break_active() ? 5U : 6U;
        break;
    case 5:
    case 6:
        outputs_.set(control_line::break_signal, request == 5);
        result.outputs_changed = true;
        break;
    case 7:
        answer = outputs_.dtr() ? 8U : 9U;
        break;
    case 8:
    case 9:
        outputs_.set(control_line::dtr, request == 8);
        result.outputs_changed = true;
        break;
    case 10:
        answer = outputs_.rts() ? 11U : 12U;
        break;
    case 11:
    case 12:
        outputs_.set(control_line::rts, request == 11);
        result.outputs_changed = true;
        break;
    case 13:
        answer = 14; // no inbound flow control
        break;
    default:
        break; // inbound and DCD, DTR and DSR flow control: acknowledged
    }
    return answer;
}

void session::command(unsigned char code, const std::string& value,
                      feed_result& result)
{
    const unsigned first =
        value.empty() ? 0U : static_cast<unsigned char>(value[0]);

    switch (code)
    {
    case cmd_signature:
        put_subnegotiation(result.reply, code, signature);
        break;
    case cmd_set_baudrate:
    {
        if (value.size() == 4)
        {
            unsigned rate = 0;
            for (const char byte : value)
                rate = (rate << 8U) | static_cast<unsigned char>(byte);
            // Zero asks for the current rate; a rate out of range is refused
            // by answering with the rate that is still in force.
            if (rate != 0 && rate <= max_baud_rate)
                settings_.baud_rate = rate;
        }
        put_subnegotiation(result.reply, code, baud_bytes(settings_.baud_rate));
        break;
    }
    case cmd_set_datasize:
        if (first >= 5 && first <= 8)
            settings_.data_bits = first;
        put_subnegotiation(result.reply, code, one_byte(settings_.data_bits));
        break;
    case cmd_set_parity:
        switch (first)
        {
        case 1:
            settings_.parity = parity_mode::none;
            break;
        case 2:
            settings_.parity = parity_mode::odd;
            break;
        case 3:
            settings_.parity = parity_mode::even;
            break;
        case 4:
            settings_.parity = parity_mode::mark;
            break;
        case 5:
            settings_.parity = parity_mode::space;
            break;
        default:
            break;
        }
        put_subnegotiation(result.reply, code,
                           one_byte(parity_code(settings_.parity)));
        break;
    case cmd_set_stopsize:
        switch (first)
        {
        case 1:
            settings_.stop_bits = stop_bits_mode::one;
            break;
        case 2:
            settings_.stop_bits = stop_bits_mode::two;
            break;
        case 3:
            settings_.stop_bits = stop_bits_mode::one_point_five;
            break;
        default:
            break;
        }
        put_subnegotiation(result.reply, code,
                           one_byte(stop_code(settings_.stop_bits)));
        break;
    case cmd_set_control:
        put_subnegotiation(result.reply, code,
                           one_byte(set_control(first, result)));
        break;
    case cmd_set_linestate_mask:
        line_mask_ = static_cast<unsigned char>(first);
        put_subnegotiation(result.reply, code, one_byte(line_mask_));
        break;
    case cmd_set_modemstate_mask:
        modem_mask_ = static_cast<unsigned char>(first);
        put_subnegotiation(result.reply, code, one_byte(modem_mask_));
        result.modem_state_requested = true;
        break;
    case cmd_purge_data:
        // Nothing is queued inside the session, so there is nothing to purge.
        put_subnegotiation(result.reply, code, one_byte(first));
        break;
    default:
        break; // flow control suspend and resume, and anything unknown
    }
}

std::string session::modem_state(control_lines inputs, bool force)
{
    VSS_PROFILE_ZONE("ports.rfc2217.modem_state");
    unsigned state = 0;
    if (inputs.cts())
        state |= 0x10U;
    if (inputs.dsr())
        state |= 0x20U;
    if (inputs.ri())
        state |= 0x40U;
    if (inputs.dcd())
        state |= 0x80U;

    unsigned deltas = 0;
    if (inputs.cts() != last_inputs_.cts())
        deltas |= 0x01U;
    if (inputs.dsr() != last_inputs_.dsr())
        deltas |= 0x02U;
    if (last_inputs_.ri() && !inputs.ri())
        deltas |= 0x04U; // trailing edge only
    if (inputs.dcd() != last_inputs_.dcd())
        deltas |= 0x08U;
    last_inputs_ = inputs;

    // The mask names the lines by their state bits; a delta bit is one nibble
    // lower, so shift the mask to compare.
    const unsigned watched = (static_cast<unsigned>(modem_mask_) >> 4U) & 0x0FU;
    if (!force && (deltas & watched) == 0)
        return {};

    std::string out;
    put_subnegotiation(out, cmd_notify_modemstate,
                       one_byte(state | deltas));
    return out;
}

} // namespace vss::rfc2217
