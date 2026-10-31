#include "ui/install_queue.hpp"
#include "ui/option_box.hpp"
#include "ui/sidebar.hpp"
#include "ui/nvg_util.hpp"
#include "app.hpp"
#include "defines.hpp"
#include "log.hpp"
#include "i18n.hpp"

#include <algorithm>
#include <cstdio>

namespace sphaira::ui {
namespace {

// the pill drawn while an install runs in the background.
constexpr float PILL_W = 420.f;
constexpr float PILL_H = 44.f;
constexpr float PILL_Y = 18.f;

} // namespace

auto InstallQueue::Get() -> InstallQueue& {
    static InstallQueue queue;
    return queue;
}

void InstallQueue::Add(const std::string& name, int image, const ProgressBoxCallback& callback) {
    if (m_shutdown) {
        return;
    }

    const auto busy = IsBusy();
    auto& job = m_jobs.emplace_back(Job{name, image, callback});
    job.id = m_next_id++;
    log_write("[QUEUE] added %s (id %zu), busy: %u\n", name.c_str(), job.id, busy);

    if (busy) {
        App::Notify(i18n::Reorder("Queued ", name) + " (" + std::to_string(GetPendingCount()) + ")");
        return;
    }

    // an idle queue opens the box like a direct install would.
    m_visible = true;
    Start(job);
}

void InstallQueue::Start(Job& job) {
    job.state = State::Running;
    m_cancel_pending = false;
    m_hide_pending = false;
    log_write("[QUEUE] starting %s (id %zu), visible: %u\n", job.name.c_str(), job.id, m_visible);

    const auto id = job.id;
    auto box = std::make_unique<ProgressBox>(job.image, "Installing "_i18n, job.name, job.callback, [this, id](Result rc) {
        // runs in the box's destructor, on the main thread.
        m_box = nullptr;
        m_finished_counter++;

        auto it = Find(id);
        if (!it) {
            return;
        }

        if (R_SUCCEEDED(rc)) {
            it->state = State::Done;
        } else if (m_cancel_pending || rc == Result_TransferCancelled) {
            it->state = State::Cancelled;
        } else {
            it->state = State::Failed;
        }
        it->rc = rc;
        m_cancel_pending = false;
        log_write("[QUEUE] finished %s (id %zu) rc: 0x%X\n", it->name.c_str(), id, rc);

        if (m_shutdown) {
            return;
        }

        if (it->state == State::Done) {
            App::Notify(i18n::Reorder("Installed ", it->name));
        } else if (it->state == State::Failed) {
            // a box being watched reports like a direct install, a hidden one
            // can't take over the screen from whatever the user is doing.
            if (m_visible) {
                App::PushErrorBox(rc, "Install failed!"_i18n);
            } else {
                char buf[32];
                std::snprintf(buf, sizeof(buf), " (0x%X)", rc);
                App::Notify("Install failed!"_i18n + " " + it->name + buf);
            }
        }
    }, ProgressBoxOption::ScreenToggle);

    box->SetHideAction([this]() {
        // the box can't take itself off the stack from inside its own update.
        m_hide_pending = true;
    });

    m_box = box.get();
    if (m_visible) {
        App::Push(std::move(box));
    } else {
        m_hidden = std::move(box);
    }
}

void InstallQueue::Update() {
    if (m_hide_pending) {
        m_hide_pending = false;
        Hide();
    }

    // a hidden box finishes where nobody pops it.
    if (m_hidden && m_hidden->ShouldExit()) {
        m_hidden.reset();
    }

    if (m_shutdown || m_box) {
        return;
    }

    auto it = std::find_if(m_jobs.begin(), m_jobs.end(), [](const auto& e) {
        return e.state == State::Waiting;
    });

    if (it != m_jobs.end()) {
        Start(*it);
    }
}

void InstallQueue::Hide() {
    if (!m_box || !m_visible) {
        return;
    }

    auto widget = App::Detach(m_box);
    if (!widget) {
        log_write("[QUEUE] running box not found on the stack\n");
        return;
    }

    m_hidden.reset(static_cast<ProgressBox*>(widget.release()));
    m_visible = false;
    App::Notify("Installing in the background"_i18n);
}

void InstallQueue::Show() {
    if (!m_hidden) {
        return;
    }

    m_visible = true;
    App::Push(std::move(m_hidden));
}

void InstallQueue::CancelRunning() {
    auto box = GetRunningBox();
    if (!box) {
        return;
    }

    m_cancel_pending = true;
    box->RequestExit();
}

void InstallQueue::Remove(u64 id) {
    std::erase_if(m_jobs, [id](const auto& e) {
        return e.id == id && e.state != State::Running;
    });
}

void InstallQueue::ClearFinished() {
    std::erase_if(m_jobs, [](const auto& e) {
        return e.state != State::Running && e.state != State::Waiting;
    });
}

void InstallQueue::Shutdown() {
    m_shutdown = true;
    std::erase_if(m_jobs, [](const auto& e) {
        return e.state == State::Waiting;
    });

    // a box on the stack is stopped and destroyed with the rest of the widgets.
    if (m_hidden) {
        m_hidden->RequestExit();
        m_hidden.reset();
    }
}

auto InstallQueue::IsBusy() const -> bool {
    return m_box || GetPendingCount();
}

auto InstallQueue::GetPendingCount() const -> size_t {
    return std::count_if(m_jobs.begin(), m_jobs.end(), [](const auto& e) {
        return e.state == State::Waiting;
    });
}

auto InstallQueue::GetRunning() -> Job* {
    auto it = std::find_if(m_jobs.begin(), m_jobs.end(), [](const auto& e) {
        return e.state == State::Running;
    });
    return it != m_jobs.end() ? &*it : nullptr;
}

auto InstallQueue::Find(u64 id) -> Job* {
    auto it = std::find_if(m_jobs.begin(), m_jobs.end(), [id](const auto& e) {
        return e.id == id;
    });
    return it != m_jobs.end() ? &*it : nullptr;
}

auto InstallQueue::GetRunningBox() const -> ProgressBox* {
    return m_box;
}

auto InstallQueue::StateName(State state) -> std::string {
    switch (state) {
        case State::Waiting: return "Waiting"_i18n;
        case State::Running: return "Installing"_i18n;
        case State::Done: return "Installed"_i18n;
        case State::Failed: return "Failed"_i18n;
        case State::Cancelled: return "Cancelled"_i18n;
    }
    return {};
}

void InstallQueue::Draw(NVGcontext* vg, Theme* theme) {
    if (!m_box || m_visible) {
        return;
    }

    auto job = GetRunning();
    if (!job) {
        return;
    }

    s64 offset{}, size{};
    m_box->GetProgress(offset, size);
    const auto percentage = size ? static_cast<u32>((double)offset / (double)size * 100.0) : 0;

    const auto total = std::count_if(m_jobs.begin(), m_jobs.end(), [](const auto& e) {
        return e.state == State::Running || e.state == State::Waiting;
    });

    const Vec4 pill{SCREEN_WIDTH / 2.f - PILL_W / 2.f, PILL_Y, PILL_W, PILL_H};
    gfx::drawRect(vg, pill, theme->GetColour(ThemeEntryID_POPUP), 22.f);

    const Vec4 bar{pill.x + 20.f, pill.y + pill.h - 9.f, pill.w - 40.f, 4.f};
    gfx::drawRect(vg, bar, theme->GetColour(ThemeEntryID_PROGRESSBAR_BACKGROUND), 2.f);
    if (size) {
        gfx::drawRect(vg, bar.x, bar.y, bar.w * ((float)offset / (float)size), bar.h, theme->GetColour(ThemeEntryID_PROGRESSBAR), 2.f);
    }

    char text[64];
    std::snprintf(text, sizeof(text), "%u%% · %zu", percentage, static_cast<size_t>(total));
    const auto right = std::string{text};

    nvgSave(vg);
    nvgIntersectScissor(vg, pill.x + 20.f, pill.y, pill.w - 110.f, pill.h);
    gfx::drawTextArgs(vg, pill.x + 20.f, pill.y + 18.f, 18.f, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT), "%s", job->name.c_str());
    nvgRestore(vg);
    gfx::drawTextArgs(vg, pill.x + pill.w - 20.f, pill.y + 18.f, 18.f, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "%s", right.c_str());
}

void InstallQueue::ShowMenu() {
    auto options = std::make_unique<Sidebar>("Install queue"_i18n, Sidebar::Side::RIGHT, 560.f);
    ON_SCOPE_EXIT(App::Push(std::move(options)));

    if (m_jobs.empty()) {
        options->Add<SidebarEntryCallback>("Nothing queued"_i18n, [](){}, "Titles you install from Ownfoil appear here."_i18n);
        return;
    }

    for (const auto& job : m_jobs) {
        const auto id = job.id;
        const auto title = "[" + StateName(job.state) + "] " + job.name;

        switch (job.state) {
            case State::Running:
                options->Add<SidebarEntryCallback>(title, [this](){
                    Show();
                }, true, "Show its progress. B there puts it back in the background, X cancels it."_i18n);
                break;

            case State::Waiting:
                options->Add<SidebarEntryCallback>(title, [this, id](){
                    Remove(id);
                }, true, "Remove it from the queue."_i18n);
                break;

            case State::Failed: {
                char info[64];
                std::snprintf(info, sizeof(info), "%s 0x%X", "Error"_i18n.c_str(), job.rc);
                options->Add<SidebarEntryCallback>(title, [](){}, std::string{info});
            }   break;

            case State::Done:
            case State::Cancelled:
                options->Add<SidebarEntryCallback>(title, [](){}, "");
                break;
        }
    }

    if (m_box) {
        options->Add<SidebarEntryCallback>("Cancel current install"_i18n, [this](){
            App::Push<OptionBox>("Cancel the current install?"_i18n, "No"_i18n, "Yes"_i18n, 1, [this](auto op_index){
                if (op_index && *op_index) {
                    CancelRunning();
                }
            });
        }, true);
    }

    if (GetPendingCount()) {
        options->Add<SidebarEntryCallback>("Remove all waiting"_i18n, [this](){
            std::erase_if(m_jobs, [](const auto& e) {
                return e.state == State::Waiting;
            });
        }, true);
    }

    if (std::any_of(m_jobs.begin(), m_jobs.end(), [](const auto& e) { return e.state != State::Running && e.state != State::Waiting; })) {
        options->Add<SidebarEntryCallback>("Clear finished"_i18n, [this](){
            ClearFinished();
        }, true);
    }
}

} // namespace sphaira::ui
