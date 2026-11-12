// "Debug logging for all NTSCRT effects" (hostparams/ntscrt_host/debug_switch.h): the marker
// file both plug-ins share, its location override and how often it is re-read.
#include <doctest/doctest.h>

#include "ntscrt_host/debug_switch.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

using namespace ntscrt::host;
namespace fs = std::filesystem;

namespace {
fs::path fresh_dir(const char *name) {
    fs::path d = fs::temp_directory_path() / "ntscrt-test-debug-switch" / name;
    std::error_code ec;
    fs::remove_all(d, ec);
    return d;
}
std::string u8(const fs::path &p) {
    const auto s = p.u8string();
    return std::string(reinterpret_cast<const char *>(s.data()), s.size());
}
void set_env(const char *k, const char *v) {
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1);
    else unsetenv(k);
#endif
}
}  // namespace

TEST_CASE("debug switch: the file in the default folder, or NTSCRT_SETTINGS_DIR") {
    set_env("NTSCRT_SETTINGS_DIR", nullptr);
    const fs::path d = fresh_dir("default");
    CHECK(debug_switch_path(u8(d)) == u8(d / kDebugSwitchFile));
    CHECK(debug_switch_path("").empty());
    const fs::path o = fresh_dir("override");
    set_env("NTSCRT_SETTINGS_DIR", u8(o).c_str());
    CHECK(debug_switch_path(u8(d)) == u8(o / kDebugSwitchFile));
    CHECK(debug_switch_path("") == u8(o / kDebugSwitchFile));
    set_env("NTSCRT_SETTINGS_DIR", nullptr);
}

TEST_CASE("debug switch: set on / off writes and removes the marker, creating the folder") {
    const fs::path d = fresh_dir("set") / "nested";
    const DebugSwitch sw(u8(d / kDebugSwitchFile));
    CHECK_FALSE(sw.on());
    CHECK(sw.set(true));
    CHECK(fs::is_regular_file(d / kDebugSwitchFile));
    CHECK(sw.on());
    CHECK(sw.refresh());
    CHECK(sw.set(false));
    CHECK_FALSE(fs::exists(d / kDebugSwitchFile));
    CHECK_FALSE(sw.on());
    CHECK(sw.set(false));  // already off
}

TEST_CASE("debug switch: another process's change is seen within the poll interval, reported once") {
    const fs::path d = fresh_dir("poll");
    const std::string file = u8(d / kDebugSwitchFile);
    const DebugSwitch sw(file, 50);
    bool changed = true;
    CHECK_FALSE(sw.on(&changed));
    CHECK_FALSE(changed);  // the first read is the initial state, not a change

    const DebugSwitch other(file);  // e.g. another host, or the tester creating the file
    REQUIRE(other.set(true));
    CHECK_FALSE(sw.on(&changed));  // still cached: no stat per call
    CHECK_FALSE(changed);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK(sw.on(&changed));
    CHECK(changed);
    CHECK(sw.on(&changed));
    CHECK_FALSE(changed);

    fs::remove(fs::u8path(file));
    CHECK_FALSE(sw.refresh(&changed));  // refresh: read now, whatever the interval
    CHECK(changed);
}

TEST_CASE("debug switch: a change seen by many threads is reported to exactly one") {
    const fs::path d = fresh_dir("threads");
    const std::string file = u8(d / kDebugSwitchFile);
    const DebugSwitch sw(file, 0);  // stat on every call
    CHECK_FALSE(sw.on());
    REQUIRE(DebugSwitch(file).set(true));
    std::atomic<int> reports{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; ++i)
        ts.emplace_back([&] {
            for (int k = 0; k < 50; ++k) {
                bool c = false;
                sw.on(&c);
                if (c) ++reports;
            }
        });
    for (auto &t : ts) t.join();
    CHECK(reports == 1);
    CHECK(sw.on());
    DebugSwitch(file).set(false);
}

TEST_CASE("debug switch: no path = always off, set fails") {
    const DebugSwitch sw("");
    CHECK_FALSE(sw.on());
    CHECK_FALSE(sw.set(true));
    CHECK(sw.path().empty());
}
