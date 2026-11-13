//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// vss_license - reads, checks and issues licence files. Offline; it has no
// network code. The commands are in tool.cpp.
//

#include "platform.hpp"
#include "tool.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    try
    {
        std::vector<std::string> arguments;
        for (int index = 1; index < argc; ++index)
            arguments.emplace_back(argv[index]);

        vss::license::tool::surroundings where{
            std::cout, std::cerr, vss::license::tool::platform::read_variable,
            std::chrono::floor<std::chrono::seconds>(
                std::chrono::system_clock::now())};
        return vss::license::tool::run(arguments, where);
    }
    catch (const std::exception& failure)
    {
        std::cerr << "vss_license: " << failure.what() << '\n';
        return vss::license::tool::exit_failed;
    }
}
