// SPDX-License-Identifier: GPL-3.0-or-later

#include "totk_netlog.h"

#include <cstdarg>
#include <cstdio>

#ifdef TOTK_NETLOG_HOST

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace TotkNetLog {
namespace {

constexpr std::size_t MaxQueue = 4096, MaxLine = 1200;

struct State {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::string> queue;
    std::thread sender, tail;
    std::atomic<bool> stop{false};
    bool started{};
    std::chrono::steady_clock::time_point t0{std::chrono::steady_clock::now()};
};

State& S() {
    static State s;
    return s;
}

void Push(std::string line) {
    auto& s = S();
    if (line.size() > MaxLine)
        line.resize(MaxLine);
    {
        std::scoped_lock lock{s.mutex};
        if (s.queue.size() >= MaxQueue)
            return;
        s.queue.push_back(std::move(line));
    }
    s.wake.notify_one();
}

void SendLoop() {
    auto& s = S();
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    // The target may be the LAN's broadcast address, so whichever PC runs the listener hears it.
    const int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(TOTK_NETLOG_PORT);
    inet_pton(AF_INET, TOTK_NETLOG_HOST, &to.sin_addr);
    // Drains the queue before a Stop() returns, so a last line before unloading still goes out.
    for (;;) {
        std::string line;
        {
            std::unique_lock lock{s.mutex};
            s.wake.wait_for(lock, std::chrono::milliseconds(500),
                            [&] { return !s.queue.empty() || s.stop.load(); });
            if (s.queue.empty()) {
                if (s.stop.load())
                    break;
                continue;
            }
            line = std::move(s.queue.front());
            s.queue.pop_front();
        }
        if (fd >= 0)
            sendto(fd, line.data(), line.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
    }
    if (fd >= 0)
        close(fd);
}

// The emulator's own log: <external files dir>/log/eden_log.txt of this app's package.
std::string EdenLogPath() {
    std::ifstream cmd{"/proc/self/cmdline", std::ios::binary};
    std::string package;
    std::getline(cmd, package, '\0');
    if (const auto colon = package.find(':'); colon != std::string::npos)
        package.resize(colon); // "pkg:service" processes
    if (package.empty())
        return {};
    return "/storage/emulated/0/Android/data/" + package + "/files/log/eden_log.txt";
}

void TailLoop() {
    auto& s = S();
    const std::string path = EdenLogPath();
    Push("[netlog] tailing " + (path.empty() ? std::string{"(no package)"} : path));
    std::FILE* f = nullptr;
    long pos = -1;
    std::string partial;
    while (!s.stop.load()) {
        if (!f && !path.empty()) {
            f = std::fopen(path.c_str(), "rb");
            if (f) {
                // Start with the last 64 KiB so the load messages before Start() come along.
                std::fseek(f, 0, SEEK_END);
                const long end = std::ftell(f);
                pos = end > 65536 ? end - 65536 : 0;
            } else {
                Push("[netlog] cannot open the emulator log");
            }
        }
        if (f) {
            std::fseek(f, 0, SEEK_END);
            const long end = std::ftell(f);
            if (end < pos)
                pos = 0; // log rotated
            if (end > pos) {
                std::string chunk(static_cast<std::size_t>(end - pos), '\0');
                std::fseek(f, pos, SEEK_SET);
                chunk.resize(std::fread(chunk.data(), 1, chunk.size(), f));
                pos += static_cast<long>(chunk.size());
                partial += chunk;
                std::size_t at;
                while ((at = partial.find('\n')) != std::string::npos) {
                    Push("[eden] " + partial.substr(0, at));
                    partial.erase(0, at + 1);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
    if (f)
        std::fclose(f);
}

} // namespace

bool Enabled() {
    return true;
}

void Start() {
    auto& s = S();
    std::scoped_lock lock{s.mutex};
    if (s.started)
        return;
    s.started = true;
    s.stop = false;
    s.sender = std::thread(SendLoop);
#ifdef __ANDROID__
    s.tail = std::thread(TailLoop);
#endif
}

void Stop() {
    auto& s = S();
    {
        std::scoped_lock lock{s.mutex};
        if (!s.started)
            return;
        s.started = false;
        s.stop = true;
    }
    s.wake.notify_all();
    if (s.sender.joinable())
        s.sender.join();
    if (s.tail.joinable())
        s.tail.join();
}

void Log(const char* fmt, ...) {
    char buf[MaxLine];
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - S().t0)
                        .count();
    const int n = std::snprintf(buf, sizeof buf, "[totk %lld.%03lld] ", static_cast<long long>(ms / 1000),
                                static_cast<long long>(ms % 1000));
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf + n, sizeof buf - static_cast<std::size_t>(n), fmt, args);
    va_end(args);
    Push(buf);
}

} // namespace TotkNetLog

#else

namespace TotkNetLog {
bool Enabled() {
    return false;
}
void Start() {}
void Stop() {}
void Log(const char*, ...) {}
} // namespace TotkNetLog

#endif
