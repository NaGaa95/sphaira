#pragma once

#include "ui/menus/menu_base.hpp"
#include "ui/list.hpp"
#include "ui/scrolling_text.hpp"
#include "yati/source/stream.hpp"

#include <atomic>
#include <memory>
#include <vector>

namespace sphaira::ui::menu::stream {

enum class State {
    // not connected.
    None,
    // just connected, starts the transfer.
    Connected,
    // set whilst transfer is in progress.
    Progress,
    // set when the transfer is finished.
    Done,
};

using OnInstallStart = std::function<bool(const char* path)>;
using OnInstallWrite = std::function<bool(const void* buf, size_t size)>;
using OnInstallClose = std::function<void()>;

struct Stream final : yati::source::Stream {
    Stream(const fs::FsPath& path, std::stop_token token);

    // called by the installer thread.
    Result ReadChunk(void* buf, s64 size, u64* bytes_read) override;

    // called by the transport thread, blocks whilst the buffer is full.
    bool Push(const void* buf, s64 size);

    // no more data will be pushed, the installer drains what is left.
    void Disable();

    // the installer is no longer reading from this stream, this makes any
    // pending (and future) Push() return immediately rather than block.
    void SetInstallFinished();

    auto& GetPath() const { return m_path; }

private:
    // waits on cv until signalled or the (short) timeout expires.
    // m_mutex must be held, no result is returned as a timeout is not an error,
    // the caller re-checks its condition on every iteration.
    void Wait(CondVar* cv);

private:
    fs::FsPath m_path{};
    std::stop_token m_token{};
    std::vector<u8> m_buffer{};
    size_t m_capacity{};
    size_t m_read_pos{};
    size_t m_write_pos{};
    size_t m_size{};
    CondVar m_can_read{};
    CondVar m_can_write{};
    Mutex m_mutex{};
    std::atomic_bool m_active{};
    std::atomic_bool m_install_done{};
};

struct Menu : MenuBase {
    Menu(const std::string& title, u32 flags);
    virtual ~Menu();

    virtual void Update(Controller* controller, TouchInfo* touch);
    virtual void Draw(NVGcontext* vg, Theme* theme);

    // tells the transport to stop accepting new installs.
    virtual void OnDisableInstallMode() = 0;
    // returns false once the transport is no longer accepting installs, used
    // to break out of the blocking callbacks below.
    virtual bool IsInstallModeActive() const = 0;

protected:
    // the below are called from the transport (mtp / ftp) thread and may block
    // for a long time, they must never be called whilst the transport holds a
    // lock that the ui thread can also take.
    bool OnInstallStart(const char* path);
    bool OnInstallWrite(const void* buf, size_t size);
    Result OnInstallClose();

    // unblocks every in-flight transport callback and disables install mode.
    // derived classes MUST call this as the first thing in their destructor,
    // before tearing the transport down, otherwise the transport thread may
    // still be sat inside one of the callbacks above.
    void CancelInstallMode();

    // true when the menu is going away or the transport stopped accepting data.
    bool IsCancelled() const;

    void SetListRect(const Vec4& v);

private:
    struct LogEntry {
        std::string name{};
        Result rc{};
    };

    // blocks until the current transfer (and its ui teardown) has completed.
    // returns false if it was cancelled instead.
    bool WaitForIdle();
    void SetIndex(s64 index);
    void UpdateSummaryHeading();

private:
    // shared_ptr as the installer thread keeps the stream alive for the
    // duration of the install, regardless of what the transport does next.
    std::shared_ptr<Stream> m_source{};
    mutable Mutex m_mutex{};
    CondVar m_idle_cv{};
    State m_state{State::None};
    Result m_last_rc{};
    bool m_screen_toggle_enabled{};

    // ui thread only.
    std::unique_ptr<ui::List> m_list{};
    std::vector<LogEntry> m_log{};
    size_t m_ok_count{};
    size_t m_fail_count{};
    s64 m_index{};
    ScrollingText m_scroll_name{};
};

} // namespace sphaira::ui::menu::stream
