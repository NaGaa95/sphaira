#include "utils/ownfoil_discovery.hpp"
#include "defines.hpp"
#include "log.hpp"

#include <algorithm>
#include <cstring>
#include <cerrno>
#include <yyjson.h>
#include <switch.h>

#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <poll.h>

namespace sphaira::ownfoil::discovery {
namespace {

constexpr u16 DISCOVERY_PORT = 8465;
constexpr const char DISCOVERY_REQUEST[] = "OWNFOIL_DISCOVER";
constexpr const char DISCOVERY_MAGIC[] = "OWNFOIL";
constexpr s64 DISCOVERY_TIMEOUT_MS = 1500;
constexpr s64 DISCOVERY_RESEND_MS = 500;

struct SocketWrapper {
    SocketWrapper(int af, int type, int proto) : sock{socket(af, type, proto)} {}
    ~SocketWrapper() {
        if (sock >= 0) {
            close(sock);
        }
    }
    operator int() const { return sock; }
    int sock{};
};

void ParseReply(const char* data, int len, const sockaddr_in& from, std::vector<DiscoveredServer>& out) {
    auto doc = yyjson_read(data, len, YYJSON_READ_NOFLAG);
    if (!doc) {
        return;
    }
    ON_SCOPE_EXIT(yyjson_doc_free(doc));

    const auto root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) {
        return;
    }

    const auto magic = yyjson_obj_get(root, "magic");
    if (!magic || !yyjson_is_str(magic) || std::strcmp(yyjson_get_str(magic), DISCOVERY_MAGIC)) {
        return;
    }

    DiscoveredServer server{};
    if (const auto v = yyjson_obj_get(root, "uid"); v && yyjson_is_str(v)) {
        server.uid = yyjson_get_str(v);
    }
    if (const auto v = yyjson_obj_get(root, "name"); v && yyjson_is_str(v)) {
        server.name = yyjson_get_str(v);
    }
    if (const auto v = yyjson_obj_get(root, "version"); v && yyjson_is_str(v)) {
        server.version = yyjson_get_str(v);
    }
    // the host is where the reply came from, never the server's own claim: behind
    // docker's NAT it can only guess at its lan address, and guesses wrong.
    if (const auto v = yyjson_obj_get(root, "port"); v && yyjson_is_uint(v)) {
        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        server.local = std::string{ip} + ":" + std::to_string(yyjson_get_uint(v));
    }
    if (const auto v = yyjson_obj_get(root, "remote"); v && yyjson_is_str(v)) {
        server.remote = yyjson_get_str(v);
    }
    if (const auto v = yyjson_obj_get(root, "public"); v && yyjson_is_bool(v)) {
        server.is_public = yyjson_get_bool(v);
    }

    if (server.uid.empty()) {
        log_write("[OWNFOIL] discovery: ignoring reply with no uid\n");
        return;
    }

    // a server might reply more than once inside the collection window.
    const auto it = std::find_if(out.begin(), out.end(), [&](auto& s) {
        return s.uid == server.uid;
    });

    if (it == out.end()) {
        out.emplace_back(std::move(server));
    } else {
        *it = std::move(server);
    }
}

// the console accepts a send to the limited broadcast then drops it, having no route, so the
// subnet-directed address is the one that carries. both are asked: which works varies by router.
auto BroadcastTargets() -> std::vector<in_addr_t> {
    std::vector<in_addr_t> targets{};

    u32 addr, mask, gateway, primary_dns, secondary_dns;
    if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&addr, &mask, &gateway, &primary_dns, &secondary_dns)) && addr && mask) {
        // nifm hands these out in network byte order already, as sin_addr wants them.
        const auto directed = addr | ~mask;
        log_write("[OWNFOIL] discovery: directed broadcast %u.%u.%u.%u\n",
            directed & 0xFF, (directed >> 8) & 0xFF, (directed >> 16) & 0xFF, (directed >> 24) & 0xFF);
        targets.emplace_back(directed);
    }

    targets.emplace_back(htonl(INADDR_BROADCAST));
    return targets;
}

} // namespace

auto Discover(std::stop_token token) -> std::vector<DiscoveredServer> {
    std::vector<DiscoveredServer> out{};

    SocketWrapper sock{AF_INET, SOCK_DGRAM, 0};
    if (sock < 0) {
        log_write("[OWNFOIL] discovery: failed to create socket: %s\n", strerror(errno));
        return out;
    }

    int enable = 1;
    if (0 > setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable))) {
        log_write("[OWNFOIL] discovery: failed to enable broadcast: %s\n", strerror(errno));
        return out;
    }

    const auto targets = BroadcastTargets();

    // a single lost datagram would mean an empty search, so the request goes out again while we wait.
    const auto ask = [&]() -> bool {
        bool sent = false;
        for (const auto target : targets) {
            sockaddr_in dest{};
            dest.sin_family = AF_INET;
            dest.sin_port = htons(DISCOVERY_PORT);
            dest.sin_addr.s_addr = target;

            if (0 > sendto(sock, DISCOVERY_REQUEST, std::strlen(DISCOVERY_REQUEST), 0, (const sockaddr*)&dest, sizeof(dest))) {
                log_write("[OWNFOIL] discovery: failed to send broadcast: %s\n", strerror(errno));
            } else {
                sent = true;
            }
        }
        return sent;
    };

    if (!ask()) {
        return out;
    }

    const auto start_ns = armTicksToNs(armGetSystemTick());
    s64 next_send_ms = DISCOVERY_RESEND_MS;
    char buf[1024];

    while (!token.stop_requested()) {
        // signed: an unsigned remaining_ms would wrap past the window instead of ending it.
        const s64 elapsed_ms = (armTicksToNs(armGetSystemTick()) - start_ns) / 1'000'000;
        const s64 remaining_ms = DISCOVERY_TIMEOUT_MS - elapsed_ms;
        if (remaining_ms <= 0) {
            break;
        }

        if (elapsed_ms >= next_send_ms) {
            ask();
            next_send_ms += DISCOVERY_RESEND_MS;
        }

        pollfd pfd{ .fd = sock, .events = POLLIN };
        const auto poll_rc = poll(&pfd, 1, static_cast<int>(std::min(remaining_ms, next_send_ms - elapsed_ms)));
        if (poll_rc < 0) {
            break;
        } else if (poll_rc == 0 || !(pfd.revents & POLLIN)) {
            continue;
        }

        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        const auto len = recvfrom(sock, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &from_len);
        if (len <= 0) {
            continue;
        }
        buf[len] = '\0';

        ParseReply(buf, len, from, out);
    }

    log_write("[OWNFOIL] discovery: found %zu server(s)\n", out.size());
    return out;
}

} // namespace sphaira::ownfoil::discovery
