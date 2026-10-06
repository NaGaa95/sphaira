#include "log.hpp"
#include "defines.hpp"
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <atomic>
#include <unistd.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <switch.h>

#if sphaira_USE_LOG
namespace {

constexpr const char* logpath = "/config/sphaira/log.txt";

std::atomic_int32_t nxlink_socket{};
std::atomic_bool g_file_open{};
Mutex g_mutex;

void log_write_arg_internal(const char* s, std::va_list* v) {
    // to the millisecond: the log is how latency gets measured on the console.
    timeval tv{};
    gettimeofday(&tv, nullptr);
    const auto t = static_cast<std::time_t>(tv.tv_sec);
    const auto tm = std::localtime(&t);

    char buf[512];
    const auto len = std::snprintf(buf, sizeof(buf), "[%02u:%02u:%02u.%03u] -> ", tm->tm_hour, tm->tm_min, tm->tm_sec, static_cast<unsigned>(tv.tv_usec / 1000));
    std::vsnprintf(buf + len, sizeof(buf) - len, s, *v);

    SCOPED_MUTEX(&g_mutex);
    if (g_file_open) {
        auto file = std::fopen(logpath, "a");
        if (file) {
            std::fprintf(file, "%s", buf);
            std::fclose(file);
        }
    }
    if (nxlink_socket) {
        std::printf("%s", buf);
    }
}

} // namespace

extern "C" {

auto log_file_init() -> bool {
    SCOPED_MUTEX(&g_mutex);
    if (g_file_open) {
        return false;
    }

    auto file = std::fopen(logpath, "w");
    if (file) {
        g_file_open = true;
        std::fclose(file);
        return true;
    }

    return false;
}

auto log_nxlink_init() -> bool {
    SCOPED_MUTEX(&g_mutex);
    if (nxlink_socket) {
        return false;
    }

    // -1 when the app wasn't launched over nxlink; kept, every log line would be
    // formatted and written to nowhere.
    const auto sock = nxlinkConnectToHost(true, false);
    if (sock < 0) {
        return false;
    }

    // every line is a small write that the next one waits on, so nagle leaves each
    // sitting on the host's delayed ack and skews the timings being measured.
    const int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    nxlink_socket = sock;
    return true;
}

void log_file_exit() {
    SCOPED_MUTEX(&g_mutex);
    if (g_file_open) {
        g_file_open = false;
    }
}

void log_nxlink_exit() {
    SCOPED_MUTEX(&g_mutex);
    if (nxlink_socket) {
        close(nxlink_socket);
        nxlink_socket = 0;
    }
}

bool log_is_init() {
    return g_file_open || nxlink_socket;
}

void log_write(const char* s, ...) {
    if (!log_is_init()) {
        return;
    }

    std::va_list v{};
    va_start(v, s);
    log_write_arg_internal(s, &v);
    va_end(v);
}

void log_write_arg(const char* s, va_list* v) {
    if (!log_is_init()) {
        return;
    }

    log_write_arg_internal(s, v);
}

} // extern "C"

#endif
