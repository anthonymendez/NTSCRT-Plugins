#include "../ntscrt_host/debug_switch.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace ntscrt::host {
namespace {
namespace fs = std::filesystem;

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string u8(const fs::path &p) {
    const auto s = p.u8string();
    return std::string(reinterpret_cast<const char *>(s.data()), s.size());
}
}  // namespace

std::string debug_switch_path(const std::string &default_dir) {
    try {
        std::string dir = default_dir;
        if (const char *e = std::getenv("NTSCRT_SETTINGS_DIR"); e && *e) dir = e;
        if (dir.empty()) return std::string();
        return u8(fs::u8path(dir) / kDebugSwitchFile);
    } catch (...) {
        return std::string();
    }
}

bool env_debug_log() {
    static const bool v = [] {
        const char *e = std::getenv("NTSCRT_DEBUG_LOG");
        if (!e || !*e) return false;
        const std::string s(e);
        return s != "0" && s != "off" && s != "OFF" && s != "false";
    }();
    return v;
}

DebugSwitch::DebugSwitch(std::string file, int64_t poll_ms) : path_(std::move(file)), poll_ns_(poll_ms * 1000000) {}

bool DebugSwitch::read_disk() const {
    if (path_.empty()) return false;
    try {
        std::error_code ec;
        return fs::is_regular_file(fs::u8path(path_), ec);
    } catch (...) {
        return false;
    }
}

bool DebugSwitch::store(bool v, bool *changed) const {
    const int old = state_.exchange(v ? 1 : 0);
    if (changed) *changed = old != -1 && old != (v ? 1 : 0);
    return v;
}

bool DebugSwitch::on(bool *changed) const {
    if (changed) *changed = false;
    const int64_t now = now_ns();
    int64_t next = next_check_.load(std::memory_order_relaxed);
    const int s = state_.load();
    if (s >= 0 && now < next) return s == 1;
    // One caller per interval stats the file; the others keep the cached state.
    if (s >= 0 && !next_check_.compare_exchange_strong(next, now + poll_ns_)) return s == 1;
    if (s < 0) next_check_.store(now + poll_ns_);
    return store(read_disk(), changed);
}

bool DebugSwitch::refresh(bool *changed) const {
    next_check_.store(now_ns() + poll_ns_);
    return store(read_disk(), changed);
}

bool DebugSwitch::set(bool on) const {
    if (path_.empty()) return false;
    try {
        const fs::path p = fs::u8path(path_);
        std::error_code ec;
        if (on) {
            fs::create_directories(p.parent_path(), ec);
            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            f << "NTSCRT: detailed debug logging is on for every NTSCRT effect (every clip, every project)\n"
                 "while this file exists. Turn it off with the effect's \"Debug logging for all effects\"\n"
                 "button, or delete this file.\n";
            f.close();
        } else {
            fs::remove(p, ec);
        }
    } catch (...) {
    }
    next_check_.store(now_ns() + poll_ns_);
    const bool now_on = read_disk();
    store(now_on, nullptr);
    return now_on == on;
}

}  // namespace ntscrt::host
