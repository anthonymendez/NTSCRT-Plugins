// file_log.h — the plug-ins' diagnostic log file (ntscrt-ofx.log, ntscrt-fxplug.log).
// Appends "<local time>.<ms> [<thread>] <msg>" lines, creating the folder; past
// max_bytes the file is renamed to "<stem>.old<ext>" (replacing the previous one)
// and a new one started. Thread-safe, never throws. An empty path disables it.
#pragma once

#include <cstdint>
#include <string>

namespace ntscrt::host {

class FileLog {
public:
    explicit FileLog(std::string path, uint64_t max_bytes = 1u << 20);
    const std::string &path() const { return path_; }  // UTF-8
    std::string folder() const;                         // parent directory ("" if none)
    void write(const std::string &msg) const;

private:
    std::string path_;
    uint64_t max_bytes_;
};

}  // namespace ntscrt::host
