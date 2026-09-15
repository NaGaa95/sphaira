#include "album_server.hpp"

#include "app.hpp"
#include "log.hpp"
#include "utils/utils.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

namespace sphaira::albumsrv {
namespace {

constexpr int LISTEN_BACKLOG = 8;
constexpr size_t MAX_REQUEST_SIZE = 1024 * 8;
constexpr size_t MAX_BODY_SIZE = 256;

// the accept loop wakes up this often to check whether it should stop.
constexpr int POLL_SLICE_MS = 100;
// how long a single read or write is given before the client is dropped.
constexpr int IO_TIMEOUT_MS = 10000;
// browsers open connections speculatively and send nothing on them, so the
// first byte of a request is given far less time than the rest.
constexpr int FIRST_BYTE_TIMEOUT_MS = 2000;

constexpr size_t ITEMS_PER_PAGE = 90;
constexpr s64 THUMBNAIL_READ_SIZE = 1024 * 96;
constexpr size_t SEND_CHUNK_SIZE = 1024 * 64;

constexpr const char* COOKIE_NAME = "sphaira_album";

auto MakePin() -> std::string {
    // rejection sampling, so that no digit is more likely than another.
    constexpr u32 LIMIT = 4294000000; // largest multiple of 1000000 that fits.

    u32 value;
    do {
        randomGet(std::addressof(value), sizeof(value));
    } while (value >= LIMIT);

    char buf[8];
    std::snprintf(buf, sizeof(buf), "%06u", value % 1000000);
    return buf;
}

auto MakeToken() -> std::string {
    u8 bytes[16];
    randomGet(bytes, sizeof(bytes));

    std::string out;
    for (const auto b : bytes) {
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%02x", b);
        out += buf;
    }

    return out;
}

// no early out, the comparison time must not depend on how much matched.
auto SecureEqual(std::string_view a, std::string_view b) -> bool {
    if (a.size() != b.size()) {
        return false;
    }

    u8 diff{};
    for (size_t i = 0; i < a.size(); i++) {
        diff |= (u8)a[i] ^ (u8)b[i];
    }

    return !diff;
}

auto Escape(std::string_view str) -> std::string {
    std::string out;
    out.reserve(str.size());

    for (const auto c : str) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c; break;
        }
    }

    return out;
}

auto EqualsIgnoreCase(std::string_view a, std::string_view b) -> bool {
    return a.size() == b.size() && !strncasecmp(a.data(), b.data(), a.size());
}

// head is everything up to (not including) the blank line.
auto FindHeader(std::string_view head, std::string_view name) -> std::string_view {
    size_t pos = head.find("\r\n");
    if (pos == std::string_view::npos) {
        return {};
    }

    pos += 2;
    while (pos < head.size()) {
        auto end = head.find("\r\n", pos);
        if (end == std::string_view::npos) {
            end = head.size();
        }

        const auto line = head.substr(pos, end - pos);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos && EqualsIgnoreCase(line.substr(0, colon), name)) {
            auto value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.remove_prefix(1);
            }
            return value;
        }

        pos = end + 2;
    }

    return {};
}

// finds key=value in a query string or a cookie header.
auto FindParam(std::string_view str, std::string_view key, char separator) -> std::string_view {
    size_t pos = 0;
    while (pos <= str.size()) {
        auto end = str.find(separator, pos);
        if (end == std::string_view::npos) {
            end = str.size();
        }

        auto pair = str.substr(pos, end - pos);
        while (!pair.empty() && pair.front() == ' ') {
            pair.remove_prefix(1);
        }

        const auto equals = pair.find('=');
        if (equals != std::string_view::npos && pair.substr(0, equals) == key) {
            return pair.substr(equals + 1);
        }

        pos = end + 1;
    }

    return {};
}

auto ParseNumber(std::string_view str, size_t& out) -> bool {
    if (str.empty() || str.size() > 9) {
        return false;
    }

    size_t value{};
    for (const auto c : str) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }

    out = value;
    return true;
}

// waits for the socket, in slices, so that the thread can still be stopped.
auto WaitReady(int fd, short events, const std::atomic_bool& stop, int timeout_ms = IO_TIMEOUT_MS) -> bool {
    for (int waited = 0; waited < timeout_ms; waited += POLL_SLICE_MS) {
        if (stop) {
            return false;
        }

        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = events;

        const auto rc = poll(std::addressof(pfd), 1, POLL_SLICE_MS);
        if (rc > 0) {
            return true;
        }

        if (rc < 0 && errno != EINTR) {
            return false;
        }
    }

    return false;
}

auto RecvSome(int fd, void* buf, size_t size, const std::atomic_bool& stop) -> ssize_t {
    while (true) {
        const auto n = recv(fd, buf, size, 0);
        if (n >= 0) {
            return n;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!WaitReady(fd, POLLIN, stop)) {
                return -1;
            }
            continue;
        }

        return -1;
    }
}

auto SendAll(int fd, const void* buf, size_t size, const std::atomic_bool& stop) -> bool {
    auto data = static_cast<const char*>(buf);

    while (size) {
        const auto n = send(fd, data, size, 0);
        if (n > 0) {
            data += n;
            size -= n;
            continue;
        }

        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!WaitReady(fd, POLLOUT, stop)) {
                return false;
            }
            continue;
        }

        return false;
    }

    return true;
}

auto SendAll(int fd, std::string_view str, const std::atomic_bool& stop) -> bool {
    return SendAll(fd, str.data(), str.size(), stop);
}

auto BuildHeader(const char* status, const char* content_type, size_t length, std::string_view extra) -> std::string {
    std::string head = "HTTP/1.1 ";
    head += status;
    head += "\r\nContent-Length: ";
    head += std::to_string(length);

    if (content_type) {
        head += "\r\nContent-Type: ";
        head += content_type;
    }

    head += extra;
    head += "\r\nConnection: close\r\n\r\n";

    return head;
}

constexpr const char* PAGE_STYLE = R"CSS(
:root { color-scheme: light dark; --bg:#f6f6f7; --fg:#1b1b1f; --card:#fff; --muted:#5f5f6a; --line:#e2e2e6; }
@media (prefers-color-scheme: dark) { :root { --bg:#16161a; --fg:#eceef2; --card:#212127; --muted:#a0a0ac; --line:#2e2e36; } }
* { box-sizing: border-box; }
body { margin:0; padding:16px; background:var(--bg); color:var(--fg);
  font:15px/1.4 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif; }
header { display:flex; flex-wrap:wrap; gap:8px; align-items:baseline;
  justify-content:space-between; margin-bottom:16px; }
h1 { font-size:18px; margin:0; }
.muted { color:var(--muted); font-size:13px; }
.grid { display:grid; gap:10px; grid-template-columns:repeat(auto-fill,minmax(150px,1fr)); }
.t { background:var(--card); border:1px solid var(--line); border-radius:10px; overflow:hidden; }
.t .p { display:block; position:relative; }
.t img { display:block; width:100%; aspect-ratio:16/9; object-fit:cover; background:var(--line); }
.t .v { position:absolute; top:6px; right:8px; padding:1px 7px; border-radius:20px; font-size:12px;
  background:rgba(0,0,0,.65); color:#fff; }
.m { display:flex; align-items:center; gap:8px; padding:8px 10px 10px; }
.m span { flex:1; min-width:0; }
.m b { display:block; font-size:13px; font-weight:600; }
.m i { display:block; font-size:12px; color:var(--muted); font-style:normal;
  white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }
.dl { flex:none; padding:6px 12px; border:1px solid var(--line); border-radius:8px; font-size:13px;
  text-decoration:none; color:inherit; background:var(--bg); }
nav { display:flex; gap:10px; justify-content:center; margin-top:20px; }
nav a, button { background:var(--card); border:1px solid var(--line); border-radius:8px;
  padding:10px 16px; color:inherit; text-decoration:none; font-size:15px; }
form { max-width:280px; margin:15vh auto 0; text-align:center; }
input { width:100%; padding:12px; font-size:22px; text-align:center; letter-spacing:6px;
  border:1px solid var(--line); border-radius:8px; background:var(--card); color:inherit; }
button { margin-top:12px; width:100%; }
)CSS";

constexpr const char* PAGE_SCRIPT = R"JS(
(function () {
  if (!navigator.canShare) return;
  document.querySelectorAll("a.dl").forEach(function (a) {
    a.textContent = "Share";
    a.addEventListener("click", function (ev) {
      ev.preventDefault();
      fetch(a.href).then(function (r) { return r.blob(); }).then(function (blob) {
        var file = new File([blob], a.getAttribute("download"), { type: blob.type });
        if (!navigator.canShare({ files: [file] })) { location.href = a.href; return; }
        navigator.share({ files: [file] }).catch(function () {});
      }).catch(function () { location.href = a.href; });
    });
  });
})();
)JS";

} // namespace

Server::Server(Items&& items, u16 port)
: m_items{std::move(items)}
, m_port{port} {
    m_pin = MakePin();
    m_token = MakeToken();

    m_thread = std::make_unique<utils::Async>([this](){
        Loop();
    });
}

Server::~Server() {
    m_stop = true;

    if (m_thread) {
        m_thread->WaitForExit();
    }
}

void Server::Loop() {
    const auto listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        log_write("[ALBUMSRV] failed to create socket: %d\n", errno);
        m_failed = true;
        return;
    }

    ON_SCOPE_EXIT(close(listen_fd));

    int enable = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, std::addressof(enable), sizeof(enable));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_fd, (const sockaddr*)std::addressof(addr), sizeof(addr)) < 0) {
        log_write("[ALBUMSRV] failed to bind to port %u: %d\n", (unsigned)m_port, errno);
        m_failed = true;
        return;
    }

    if (listen(listen_fd, LISTEN_BACKLOG) < 0) {
        log_write("[ALBUMSRV] failed to listen: %d\n", errno);
        m_failed = true;
        return;
    }

    log_write("[ALBUMSRV] listening on port %u\n", (unsigned)m_port);
    m_listening = true;

    while (!m_stop) {
        pollfd pfd{};
        pfd.fd = listen_fd;
        pfd.events = POLLIN;

        const auto rc = poll(std::addressof(pfd), 1, POLL_SLICE_MS);
        if (rc <= 0) {
            continue;
        }

        const auto fd = accept(listen_fd, nullptr, nullptr);
        if (fd < 0) {
            continue;
        }

        // every read and write polls, so a stalled client can never keep the
        // thread from noticing that it should stop.
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

        HandleConnection(fd);

        shutdown(fd, SHUT_RDWR);
        close(fd);
    }

    m_listening = false;
    log_write("[ALBUMSRV] stopped\n");
}

void Server::HandleConnection(int fd) {
    // nothing is served in parallel, so a connection that never says anything
    // must not be allowed to hold the loop up.
    if (!WaitReady(fd, POLLIN, m_stop, FIRST_BYTE_TIMEOUT_MS)) {
        return;
    }

    std::string request;

    // read up to the end of the headers.
    for (;;) {
        if (request.find("\r\n\r\n") != std::string::npos) {
            break;
        }

        if (request.size() >= MAX_REQUEST_SIZE) {
            return;
        }

        char buf[1024];
        const auto n = RecvSome(fd, buf, sizeof(buf), m_stop);
        if (n <= 0) {
            return;
        }

        request.append(buf, n);
    }

    const auto head_end = request.find("\r\n\r\n");
    const std::string_view head{request.data(), head_end};

    const auto line_end = head.find("\r\n");
    const auto line = line_end == std::string_view::npos ? head : head.substr(0, line_end);

    const auto method_end = line.find(' ');
    if (method_end == std::string_view::npos) {
        return;
    }

    const auto target_end = line.find(' ', method_end + 1);
    if (target_end == std::string_view::npos) {
        return;
    }

    const auto method = line.substr(0, method_end);
    const auto target = line.substr(method_end + 1, target_end - method_end - 1);

    auto path = target;
    std::string_view query;
    if (const auto mark = target.find('?'); mark != std::string_view::npos) {
        path = target.substr(0, mark);
        query = target.substr(mark + 1);
    }

    m_requests++;
    App::NotifyFlashLed();

    // the pin can be handed over as a form post, or in the link the QR code
    // carries, so that scanning it is all a phone has to do.
    if (method == "POST" && path == "/auth") {
        auto body = std::string_view{request}.substr(head_end + 4);

        size_t length{};
        if (ParseNumber(FindHeader(head, "Content-Length"), length) && length <= MAX_BODY_SIZE) {
            while (body.size() < length) {
                char buf[MAX_BODY_SIZE];
                const auto n = RecvSome(fd, buf, std::min(sizeof(buf), length - body.size()), m_stop);
                if (n <= 0) {
                    break;
                }
                request.append(buf, n);
                body = std::string_view{request}.substr(head_end + 4);
            }
        }

        HandleAuth(fd, FindParam(body, "pin", '&'));
        return;
    }

    if (method != "GET") {
        const auto head_str = BuildHeader("405 Method Not Allowed", "text/plain", 0, "");
        SendAll(fd, head_str, m_stop);
        return;
    }

    const auto cookie = FindParam(FindHeader(head, "Cookie"), COOKIE_NAME, ';');
    if (!SecureEqual(cookie, m_token)) {
        // the link in the QR code carries the pin, so that scanning it is all
        // a phone has to do. the cookie is set and the pin bounced out of the
        // address bar.
        const auto pin = FindParam(query, "p", '&');
        if (pin.empty()) {
            HandleLogin(fd);
        } else {
            HandleAuth(fd, pin);
        }
        return;
    }

    if (path == "/") {
        HandleGallery(fd, query);
        return;
    }

    // ?dl=1 is what the save button asks for.
    const auto attachment = FindParam(query, "dl", '&') == "1";

    const auto serve_item = [this, fd, attachment](std::string_view index_str, bool thumbnail) {
        size_t index{};
        if (!ParseNumber(index_str, index) || index >= m_items.size()) {
            const auto head_str = BuildHeader("404 Not Found", "text/plain", 0, "");
            SendAll(fd, head_str, m_stop);
            return;
        }

        if (thumbnail) {
            HandleThumbnail(fd, m_items[index]);
        } else {
            HandleFile(fd, m_items[index], attachment);
        }
    };

    if (path.starts_with("/thumb/")) {
        serve_item(path.substr(std::strlen("/thumb/")), true);
        return;
    }

    if (path.starts_with("/file/")) {
        serve_item(path.substr(std::strlen("/file/")), false);
        return;
    }

    const auto head_str = BuildHeader("404 Not Found", "text/plain", 0, "");
    SendAll(fd, head_str, m_stop);
}

void Server::HandleAuth(int fd, std::string_view pin) {
    // a six digit pin is only worth anything if it cannot be worked through.
    if (IsLockedOut()) {
        const auto head = BuildHeader("403 Forbidden", "text/plain", 0, "");
        SendAll(fd, head, m_stop);
        return;
    }

    if (!SecureEqual(pin, m_pin)) {
        m_auth_failures++;
        log_write("[ALBUMSRV] wrong pin (%u)\n", m_auth_failures.load());
        HandleLogin(fd);
        return;
    }

    std::string extra = "\r\nSet-Cookie: ";
    extra += COOKIE_NAME;
    extra += "=";
    extra += m_token;
    extra += "; Path=/; HttpOnly; SameSite=Strict";
    extra += "\r\nLocation: /";

    const auto head = BuildHeader("303 See Other", nullptr, 0, extra);
    SendAll(fd, head, m_stop);
}

void Server::HandleLogin(int fd) {
    std::string body = "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Sphaira Album</title><style>";
    body += PAGE_STYLE;
    body += "</style></head><body><form method=\"post\" action=\"/auth\">"
        "<h1>Sphaira Album</h1>"
        "<p class=\"muted\">Enter the PIN shown on the console.</p>"
        "<input name=\"pin\" inputmode=\"numeric\" pattern=\"[0-9]*\" maxlength=\"6\" required autofocus>"
        "<button type=\"submit\">Open</button></form></body></html>";

    const auto head = BuildHeader("200 OK", "text/html; charset=utf-8", body.size(), "");
    if (SendAll(fd, head, m_stop)) {
        SendAll(fd, body, m_stop);
    }
}

void Server::HandleGallery(int fd, std::string_view query) {
    size_t page{};
    if (!ParseNumber(FindParam(query, "page", '&'), page)) {
        page = 0;
    }

    const auto body = BuildGalleryPage(page);
    const auto head = BuildHeader("200 OK", "text/html; charset=utf-8", body.size(), "");

    if (SendAll(fd, head, m_stop)) {
        SendAll(fd, body, m_stop);
    }
}

auto Server::BuildGalleryPage(size_t page) const -> std::string {
    const auto pages = m_items.empty() ? 1 : (m_items.size() + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
    page = std::min(page, pages - 1);

    const auto first = page * ITEMS_PER_PAGE;
    const auto last = std::min(first + ITEMS_PER_PAGE, m_items.size());

    std::string out = "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Sphaira Album</title><style>";
    out += PAGE_STYLE;
    out += "</style></head><body><header><h1>Sphaira Album</h1><span class=\"muted\">";
    out += std::to_string(m_items.size());
    out += " captures &middot; page ";
    out += std::to_string(page + 1);
    out += " of ";
    out += std::to_string(pages);
    out += "</span></header><div class=\"grid\">";

    for (size_t i = first; i < last; i++) {
        const auto& item = m_items[i];
        const auto index = std::to_string(i);

        out += "<div class=\"t\"><a class=\"p\" href=\"/file/";
        out += index;
        out += "\"><img loading=\"lazy\" src=\"/thumb/";
        out += index;
        out += "\" alt=\"\">";

        if (item.video) {
            out += "<span class=\"v\">&#9654;</span>";
        }

        out += "</a><div class=\"m\"><span><b>";
        out += Escape(item.name);
        out += "</b><i>";

        if (!item.title.empty()) {
            out += Escape(item.title);
            out += " &middot; ";
        }

        out += utils::formatSizeStorage(item.size);
        out += "</i></span><a class=\"dl\" href=\"/file/";
        out += index;
        out += "?dl=1\" download=\"";
        out += Escape(item.file);
        out += "\">Save</a></div></div>";
    }

    out += "</div><nav>";

    if (page) {
        out += "<a href=\"/?page=";
        out += std::to_string(page - 1);
        out += "\">&larr; Newer</a>";
    }

    if (page + 1 < pages) {
        out += "<a href=\"/?page=";
        out += std::to_string(page + 1);
        out += "\">Older &rarr;</a>";
    }

    out += "</nav><script>";
    out += PAGE_SCRIPT;
    out += "</script></body></html>";
    return out;
}

void Server::HandleThumbnail(int fd, const Item& item) {
    // the console keeps a 320x180 jpeg for every capture, which is exactly
    // what a grid on a phone wants.
    std::vector<u8> buf;
    if (R_FAILED(caps::LoadThumbnail(item.file_id, buf)) || buf.empty()) {
        const auto head = BuildHeader("404 Not Found", "text/plain", 0, "");
        SendAll(fd, head, m_stop);
        return;
    }

    const auto head = BuildHeader("200 OK", "image/jpeg", buf.size(), "\r\nCache-Control: max-age=3600");
    if (SendAll(fd, head, m_stop)) {
        SendAll(fd, buf.data(), buf.size(), m_stop);
    }
}

void Server::HandleFile(int fd, const Item& item, bool attachment) {
    // the name ends up inside a quoted header value.
    std::string name;
    for (const auto c : item.file) {
        name += (c == '"' || c == '\\' || (unsigned char)c < 0x20) ? '_' : c;
    }

    std::string extra = "\r\nContent-Disposition: ";
    extra += attachment ? "attachment" : "inline";
    extra += "; filename=\"";
    extra += name;
    extra += "\"";

    // a clip is tens of megabytes, so it is streamed rather than loaded.
    if (item.video) {
        caps::MovieStream stream;
        if (R_SUCCEEDED(stream.Open(item.file_id))) {
            u64 size{};
            if (R_SUCCEEDED(stream.GetSize(std::addressof(size))) && size) {
                const auto head = BuildHeader("200 OK", "video/mp4", size, extra);
                if (SendAll(fd, head, m_stop)) {
                    SendMovie(fd, stream, size);
                }
                return;
            }
        }

        // below [4.0.0+] there is no stream, fall through and load the lot.
    }

    std::vector<u8> buf;
    if (R_FAILED(caps::LoadFile(item.file_id, buf)) || buf.empty()) {
        const auto head = BuildHeader("404 Not Found", "text/plain", 0, "");
        SendAll(fd, head, m_stop);
        return;
    }

    const auto head = BuildHeader("200 OK", item.video ? "video/mp4" : "image/jpeg", buf.size(), extra);
    if (SendAll(fd, head, m_stop)) {
        SendAll(fd, buf.data(), buf.size(), m_stop);
    }
}

void Server::SendMovie(int fd, caps::MovieStream& stream, u64 size) {
    // reads have to be aligned, and the last one is zero padded out to the
    // alignment, so only what the stream says is real gets sent.
    std::vector<u8> buf(caps::MovieStream::ALIGNMENT);

    for (u64 offset = 0; offset < size;) {
        u64 read{};
        if (R_FAILED(stream.Read(offset, buf.data(), buf.size(), std::addressof(read))) || !read) {
            return;
        }

        const auto chunk = std::min<u64>(read, size - offset);
        if (!SendAll(fd, buf.data(), chunk, m_stop)) {
            return;
        }

        offset += chunk;
    }
}

} // namespace sphaira::albumsrv
