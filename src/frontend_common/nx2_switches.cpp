// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <utility>

#include "common/fs/path_util.h"
#include "frontend_common/nx2_switches.h"

namespace FrontendCommon {
namespace {

void ParseSwitchFile(const std::filesystem::path& path,
                     std::map<std::string, std::string>& entries) {
    std::ifstream file{path};
    if (!file) {
        return;
    }
    constexpr const char* blank = " \t\r";
    std::string line;
    bool first_line = true;
    while (std::getline(file, line)) {
        if (first_line && line.starts_with("\xEF\xBB\xBF")) {
            line.erase(0, 3);
        }
        first_line = false;
        line.erase(0, line.find_first_not_of(blank));
        line.erase(line.find_last_not_of(blank) + 1);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string name = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        name.erase(name.find_last_not_of(blank) + 1);
        value.erase(0, value.find_first_not_of(blank));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        if (!name.empty()) {
            entries.insert_or_assign(std::move(name), std::move(value));
        }
    }
}

std::filesystem::path GetSwitchExeDirectory(const char* argv0) {
#ifdef _WIN32
    return Common::FS::GetExeDirectory();
#else
    std::error_code ec;
#ifdef __linux__
    if (auto exe = std::filesystem::read_symlink("/proc/self/exe", ec); !ec) {
        return exe.parent_path();
    }
#endif
    const auto exe = std::filesystem::absolute(argv0, ec);
    return ec ? std::filesystem::path{} : exe.parent_path();
#endif
}

} // namespace

void LoadNx2Switches(const char* argv0, const std::filesystem::path& config_dir) {
    std::map<std::string, std::string> entries;
    if (const auto exe_dir = GetSwitchExeDirectory(argv0); !exe_dir.empty()) {
        ParseSwitchFile(exe_dir / "nx2_switches.txt", entries);
    }
    ParseSwitchFile(config_dir / "nx2_switches.txt", entries);
    for (const auto& [name, value] : entries) {
        if (std::getenv(name.c_str()) != nullptr) {
            continue;
        }
#ifdef _WIN32
        _putenv_s(name.c_str(), value.c_str());
#else
        setenv(name.c_str(), value.c_str(), 1);
#endif
    }
}

} // namespace FrontendCommon
