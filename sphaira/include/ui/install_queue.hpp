#pragma once

#include "ui/widget.hpp"
#include "ui/progress_box.hpp"

#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace sphaira::ui {

// installs run one after the other. the running one's progress box can be put
// away with B and the install carries on in the background, while the next
// titles are queued from wherever the user goes.
//
// everything here is touched from the main thread only: the progress box's own
// thread only ever reaches the queue through the box's done callback, which runs
// in the box's destructor - and that is always destroyed on the main thread.
class InstallQueue final {
public:
    enum class State { Waiting, Running, Done, Failed, Cancelled };

    struct Job {
        std::string name{};
        int image{};                   // shown in the progress box, may be 0.
        ProgressBoxCallback callback{};
        State state{State::Waiting};
        Result rc{};
        u64 id{};
    };

    static auto Get() -> InstallQueue&;

    // queues a job, starting it right away when nothing runs. the first job of
    // an idle queue opens its progress box, the rest start where the previous
    // one was left: on screen if it was being watched, in the background if not.
    void Add(const std::string& name, int image, const ProgressBoxCallback& callback);

    // called once per frame by the app, after the widgets have updated.
    void Update();
    // draws the background status pill when a hidden install is running.
    void Draw(NVGcontext* vg, Theme* theme);
    // stops the running install and drops what waits, on exit.
    void Shutdown();

    // true while an install runs or waits.
    auto IsBusy() const -> bool;
    auto GetPendingCount() const -> size_t;
    // bumped every time a job finishes, for pages that show what is installed.
    auto GetFinishedCounter() const -> u64 { return m_finished_counter; }

    // the queue screen.
    void ShowMenu();

private:
    void Start(Job& job);
    void Show();
    void Hide();
    void CancelRunning();
    void Remove(u64 id);
    void ClearFinished();
    auto GetRunning() -> Job*;
    auto Find(u64 id) -> Job*;
    auto GetRunningBox() const -> ProgressBox*;
    static auto StateName(State state) -> std::string;

private:
    std::deque<Job> m_jobs{};
    // the running job's box, while it is off the widget stack.
    std::unique_ptr<ProgressBox> m_hidden{};
    // the running job's box, wherever it lives. null when nothing runs.
    ProgressBox* m_box{};
    u64 m_next_id{1};
    u64 m_finished_counter{};
    bool m_visible{};        // the running box is on the widget stack.
    bool m_hide_pending{};   // B was pressed on the box, detach it next update.
    bool m_cancel_pending{}; // the running job was cancelled by the user.
    bool m_shutdown{};
};

} // namespace sphaira::ui
