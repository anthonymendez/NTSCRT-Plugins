// debug_switch.h — "Debug logging for all NTSCRT effects": one switch per user and machine,
// outside any project, shared by the OpenFX and FxPlug plug-ins.
//
// The switch is a marker file, kDebugSwitchFile, in the settings folder: the plug-in's log
// folder by default (Windows %LOCALAPPDATA%\NTSCRT, macOS ~/Library/Logs/NTSCRT, Final Cut:
// <renderer container>/Data/Library/Logs/NTSCRT). It is on while the file exists, so a tester
// can also create or delete it by hand. NTSCRT_SETTINGS_DIR=<folder> moves it (tests).
// Effects already open notice a change within about a second: on() stats the file at most once
// per poll interval, whatever the number of calls or threads. Thread-safe, never throws.
//
// An effect logs in detail when its own "Debug logging" checkbox is on, OR this switch is on,
// OR NTSCRT_DEBUG_LOG=1 is set in the host's environment (env_debug_log()).
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace ntscrt::host {

constexpr const char kDebugSwitchFile[] = "debug-logging-all-effects.txt";

// The switch file in `default_dir` (UTF-8), or in $NTSCRT_SETTINGS_DIR when that is set.
// "" when both are empty (switch unavailable: always off).
std::string debug_switch_path(const std::string &default_dir);

// NTSCRT_DEBUG_LOG set to anything but "", "0" or "off" (read once per process).
bool env_debug_log();

class DebugSwitch {
public:
    explicit DebugSwitch(std::string file, int64_t poll_ms = 1000);
    const std::string &path() const { return path_; }  // UTF-8, "" = unavailable

    // Current state, re-read from disk at most once per poll interval. *changed (optional) is
    // set true for exactly one caller per observed change (to log it once per process).
    bool on(bool *changed = nullptr) const;
    // Re-read now (panel opened, button pressed).
    bool refresh(bool *changed = nullptr) const;
    // Turn it on (write the marker file, creating the folder) or off (delete it). Returns
    // false when that failed; the cached state then holds what is on disk.
    bool set(bool on) const;

private:
    bool read_disk() const;
    bool store(bool v, bool *changed) const;

    std::string path_;
    int64_t poll_ns_;
    mutable std::atomic<int64_t> next_check_{INT64_MIN};  // steady_clock ns
    mutable std::atomic<int> state_{-1};                   // -1 unknown, 0 off, 1 on
};

}  // namespace ntscrt::host
