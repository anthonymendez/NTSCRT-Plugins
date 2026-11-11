#include "../ntscrt_host/file_log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace ntscrt::host {

FileLog::FileLog(std::string path, uint64_t max_bytes) : path_(std::move(path)), max_bytes_(max_bytes) {}

std::string FileLog::folder() const {
    if (path_.empty()) return std::string();
    const auto s = std::filesystem::u8path(path_).parent_path().u8string();
    return std::string(reinterpret_cast<const char *>(s.data()), s.size());
}

void FileLog::write(const std::string &msg) const {
    if (path_.empty()) return;
    try {
        namespace fs = std::filesystem;
        static std::mutex mu;
        std::lock_guard<std::mutex> lk(mu);
        const fs::path p = fs::u8path(path_);
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        if (fs::exists(p, ec) && fs::file_size(p, ec) > max_bytes_) {
            fs::path old = p;
            old.replace_extension();
            old += ".old";
            old += p.extension();
            fs::remove(old, ec);
            fs::rename(p, old, ec);
        }
        std::ofstream f(p, std::ios::app);
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char ts[32];
        std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
        char msbuf[8];
        std::snprintf(msbuf, sizeof msbuf, ".%03d", ms);
        std::ostringstream tid;
        tid << std::this_thread::get_id();
        f << ts << msbuf << " [" << tid.str() << "] " << msg << '\n';
    } catch (...) {
    }
}

}  // namespace ntscrt::host
