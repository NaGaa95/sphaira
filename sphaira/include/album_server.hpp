#pragma once

#include "album_caps.hpp"
#include "utils/thread.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace sphaira::albumsrv {

// a snapshot of the album, taken when the server starts. the server only ever
// addresses entries by their index into this list, so nothing from a request
// is ever used to look a capture up.
struct Item {
    CapsAlbumFileId file_id{};
    std::string name{};  // date and time of the capture.
    std::string title{}; // the game it was taken in, where that is known.
    std::string file{};  // file name, used for the download.
    u64 size{};
    bool video{};
};

using Items = std::vector<Item>;

// serves the album over http, read only, for as long as it is alive.
struct Server final {
    Server(Items&& items, u16 port);
    ~Server();

    Server(const Server&) = delete;
    void operator=(const Server&) = delete;

    auto GetPort() const -> u16 {
        return m_port;
    }

    auto GetPin() const -> const std::string& {
        return m_pin;
    }

    // set once the socket is listening.
    auto IsListening() const -> bool {
        return m_listening;
    }

    // set if the socket could not be opened, bound or listened on.
    auto IsFailed() const -> bool {
        return m_failed;
    }

    auto GetRequestCount() const -> u32 {
        return m_requests;
    }

    // set once too many pins have been guessed, the screen has to be closed
    // and re-opened (with a new pin) to try again.
    auto IsLockedOut() const -> bool {
        return m_auth_failures >= MAX_AUTH_FAILURES;
    }

private:
    static constexpr u32 MAX_AUTH_FAILURES = 10;

    void Loop();
    void HandleConnection(int fd);

    void HandleGallery(int fd, std::string_view query);
    void HandleLogin(int fd);
    void HandleAuth(int fd, std::string_view pin);
    void HandleThumbnail(int fd, const Item& item);
    // attachment asks the browser to save rather than display it.
    void HandleFile(int fd, const Item& item, bool attachment);
    void SendMovie(int fd, caps::MovieStream& stream, u64 size);

    auto BuildGalleryPage(size_t page) const -> std::string;

private:
    const Items m_items;
    const u16 m_port;

    // shown on screen only, exchanged by a client for the session token.
    std::string m_pin{};
    std::string m_token{};

    std::atomic_bool m_stop{};
    std::atomic_bool m_listening{};
    std::atomic_bool m_failed{};
    std::atomic<u32> m_requests{};
    std::atomic<u32> m_auth_failures{};

    std::unique_ptr<utils::Async> m_thread{};
};

} // namespace sphaira::albumsrv
