#pragma once

#include "ui/widget.hpp"
#include "ui/scrolling_text.hpp"
#include "fs.hpp"
#include <functional>
#include <span>

namespace sphaira::ui {

struct ProgressBox;
using ProgressBoxCallback = std::function<Result(ProgressBox*)>;
using ProgressBoxDoneCallback = std::function<void(Result rc)>;
// using CancelCallback = std::function<void()>;

enum class ProgressBoxOption : u8 {
    None,
    ScreenToggle,
};

struct ProgressBox final : Widget {
    ProgressBox(
        int image,
        const std::string& action,
        const std::string& title,
        const ProgressBoxCallback& callback,
        const ProgressBoxDoneCallback& done = nullptr,
        ProgressBoxOption option = ProgressBoxOption::None
    );
    ~ProgressBox();

    auto Update(Controller* controller, TouchInfo* touch) -> void override;
    auto Draw(NVGcontext* vg, Theme* theme) -> void override;

    auto SetActionName(const std::string& action) -> ProgressBox&;
    auto SetTitle(const std::string& title) -> ProgressBox&;
    auto NewTransfer(const std::string& transfer) -> ProgressBox&;
    // zeros the saved offset.
    auto ResetTranfser() -> ProgressBox&;
    auto UpdateTransfer(s64 offset, s64 size) -> ProgressBox&;
    // not const in order to avoid copy by using std::swap
    auto SetImage(int image) -> ProgressBox&;
    auto SetImageData(std::vector<u8>& data) -> ProgressBox&;
    auto SetImageDataConst(std::span<const u8> data) -> ProgressBox&;

    // B puts the box away through on_hide instead of cancelling, X cancels.
    // used by the install queue, which keeps the box running off the stack.
    void SetHideAction(const std::function<void()>& on_hide);
    // thread safe, for drawing the progress of a box that isn't on screen.
    void GetProgress(s64& offset, s64& size);

    void RequestExit();
    auto ShouldExit() -> bool;
    auto ShouldExitResult() -> Result;

    void AddCancelEvent(UEvent* event);
    void RemoveCancelEvent(const UEvent* event);

    // helper functions
    auto CopyFile(fs::Fs* fs_src, fs::Fs* fs_dst, const fs::FsPath& src, const fs::FsPath& dst, bool single_threaded = false) -> Result;
    auto CopyFile(fs::Fs* fs, const fs::FsPath& src, const fs::FsPath& dst, bool single_threaded = false) -> Result;
    auto CopyFile(const fs::FsPath& src, const fs::FsPath& dst, bool single_threaded = false) -> Result;
    void Yield();

    auto OnDownloadProgressCallback() {
        return [this](s64 dltotal, s64 dlnow, s64 ultotal, s64 ulnow){
            if (this->ShouldExit()) {
                return false;
            }

            if (dltotal) {
                this->UpdateTransfer(dlnow, dltotal);
            } else {
                this->UpdateTransfer(ulnow, ultotal);
            }

            return true;
        };
    }

    // auto-clear = false
    auto GetCancelEvent() {
        return &m_uevent;
    }

private:
    void FreeImage();
    auto GetFooterButtons() const -> Widget::uiButtons;
    auto HasFooter() const -> bool;
    void SetupPosition();

public:
    struct ThreadData {
        ProgressBox* pbox{};
        ProgressBoxCallback callback{};
        Result result{};
    };

private:
    UEvent m_uevent{};
    Mutex m_mutex{};
    Thread m_thread{};
    ThreadData m_thread_data{};
    ProgressBoxDoneCallback m_done{};
    std::vector<UEvent*> m_cancel_events{};

    // shared data start.
    std::string m_action{};
    std::string m_title{};
    std::string m_transfer{};
    s64 m_size{};
    s64 m_offset{};
    s64 m_last_offset{};
    s64 m_speed{};
    TimeStamp m_timestamp{};
    std::vector<u8> m_image_data{};
    int m_image_pending{};
    bool m_is_image_pending{};
    // shared data end.

    ScrollingText m_scroll_title{};
    ScrollingText m_scroll_transfer{};

    int m_image{};
    bool m_own_image{};
    bool m_screen_toggle_enabled{};
    bool m_hideable{};
};

// this is a helper function that does many things.
// 1. creates a progress box, pushes that box to app
// 2. creates a thread and passes the pbox and callback to that thread
// 3. that thread calls the callback.

// this allows for blocking processes to run on a seperate thread whilst
// updating the ui with the progress of the operation.
// the callback should poll ShouldExit() whether to keep running

} // namespace sphaira::ui {
