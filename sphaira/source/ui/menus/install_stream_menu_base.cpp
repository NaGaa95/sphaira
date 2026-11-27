#include "ui/menus/install_stream_menu_base.hpp"
#include "yati/yati.hpp"
#include "app.hpp"
#include "defines.hpp"
#include "log.hpp"
#include "ui/nvg_util.hpp"
#include "i18n.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace sphaira::ui::menu::stream {
namespace {

constexpr u64 MAX_BUFFER_SIZE_APP = 1024ULL * 1024ULL * 16ULL;
constexpr u64 MAX_BUFFER_SIZE_APPLET = 1024ULL * 1024ULL * 4ULL;
constexpr size_t MAX_LOG_ENTRIES = 500;

// every wait in here is bounded, a missed wakeup then costs a few ms rather
// than hanging the transport (and with it the whole app) forever.
constexpr u64 WAIT_TIMEOUT = 1e+8; // 100ms.

} // namespace

Stream::Stream(const fs::FsPath& path, std::stop_token token) {
    m_path = path;
    m_token = token;
    m_active = true;
    m_capacity = App::IsApplication() ? MAX_BUFFER_SIZE_APP : MAX_BUFFER_SIZE_APPLET;
    m_buffer.resize(m_capacity);

    mutexInit(&m_mutex);
    condvarInit(&m_can_read);
    condvarInit(&m_can_write);
}

void Stream::Wait(CondVar* cv) {
    // a timeout is expected and not an error, the caller loops and re-checks.
    condvarWaitTimeout(cv, std::addressof(m_mutex), WAIT_TIMEOUT);
}

Result Stream::ReadChunk(void* _buf, s64 size, u64* bytes_read) {
    auto buf = static_cast<u8*>(_buf);
    *bytes_read = 0;

    while (!m_token.stop_requested()) {
        SCOPED_MUTEX(&m_mutex);

        if (!m_size) {
            if (!m_active) {
                break;
            }

            Wait(std::addressof(m_can_read));
            continue;
        }

        const auto rsize = std::min<size_t>(size, m_size);
        const auto first = std::min<size_t>(rsize, m_capacity - m_read_pos);
        std::memcpy(buf, m_buffer.data() + m_read_pos, first);
        if (rsize > first) {
            std::memcpy(buf + first, m_buffer.data(), rsize - first);
        }

        m_read_pos = (m_read_pos + rsize) % m_capacity;
        m_size -= rsize;
        condvarWakeAll(std::addressof(m_can_write));

        size -= rsize;
        buf += rsize;
        *bytes_read += rsize;

        if (!size) {
            R_SUCCEED();
        }
    }

    // a short read is still a success, yati treats zero bytes read as a
    // cancelled transfer.
    if (*bytes_read) {
        R_SUCCEED();
    }

    log_write("[Stream::ReadChunk] failed to read\n");
    R_THROW(Result_TransferCancelled);
}

bool Stream::Push(const void* _buf, s64 size) {
    auto buf = static_cast<const u8*>(_buf);
    if (!size) {
        return true;
    }

    while (!m_token.stop_requested()) {
        SCOPED_MUTEX(&m_mutex);

        // the installer already finished (or gave up) reading this file, the
        // remaining data is not needed, pretend it was consumed so that the
        // transport can drain cleanly and move on to the next file.
        if (m_install_done) {
            return true;
        }

        if (!m_active) {
            log_write("[Stream::Push] file not active\n");
            break;
        }

        if (m_size >= m_capacity) {
            Wait(std::addressof(m_can_write));
            continue;
        }

        const auto wsize = std::min<size_t>(size, m_capacity - m_size);
        const auto first = std::min<size_t>(wsize, m_capacity - m_write_pos);
        std::memcpy(m_buffer.data() + m_write_pos, buf, first);
        if (wsize > first) {
            std::memcpy(m_buffer.data(), buf + first, wsize - first);
        }

        m_write_pos = (m_write_pos + wsize) % m_capacity;
        m_size += wsize;
        condvarWakeAll(std::addressof(m_can_read));

        size -= wsize;
        buf += wsize;
        if (!size) {
            return true;
        }
    }

    log_write("[Stream::Push] failed to push\n");
    return false;
}

void Stream::Disable() {
    log_write("[Stream::Disable] disabling file\n");

    SCOPED_MUTEX(&m_mutex);
    m_active = false;
    condvarWakeAll(std::addressof(m_can_read));
    condvarWakeAll(std::addressof(m_can_write));
}

void Stream::SetInstallFinished() {
    SCOPED_MUTEX(&m_mutex);
    m_install_done = true;
    m_active = false;
    m_size = 0;
    m_read_pos = 0;
    m_write_pos = 0;
    condvarWakeAll(std::addressof(m_can_read));
    condvarWakeAll(std::addressof(m_can_write));
}

Menu::Menu(const std::string& title, u32 flags) : MenuBase{title, flags} {
    SetAction(Button::B, Action{"Back"_i18n, [this](){
        SetPop();
    }});

    SetAction(Button::X, Action{"Options"_i18n, [this](){
        App::DisplayInstallOptions(false);
    }});

    SetListRect({75.f, GetY() + 1.f + 42.f, 1220.f - 45.f * 2.f, 60.f});

    // hold a screen-toggle reference across the session so turning the screen
    // off survives transitioning between files in a batch.
    if (App::AcquireScreenToggle()) {
        m_screen_toggle_enabled = true;
    }

    App::SetAutoSleepDisabled(true);
    mutexInit(&m_mutex);
    condvarInit(&m_idle_cv);
}

Menu::~Menu() {
    // derived destructors already called CancelInstallMode(), this is only a
    // safety net in case one of them forgot to.
    m_stop_source.request_stop();

    std::shared_ptr<Stream> source;
    {
        SCOPED_MUTEX(&m_mutex);
        source = m_source;
        condvarWakeAll(&m_idle_cv);
    }

    if (source) {
        source->Disable();
        source->SetInstallFinished();
    }

    if (m_screen_toggle_enabled) {
        App::ReleaseScreenToggle();
        m_screen_toggle_enabled = false;
    }

    App::SetAutoSleepDisabled(false);
}

bool Menu::IsCancelled() const {
    // check the cheap, non-virtual condition first, the transport may already
    // be halfway through tearing us down.
    if (GetToken().stop_requested()) {
        return true;
    }

    return !IsInstallModeActive();
}

void Menu::CancelInstallMode() {
    // 1. unblock the transport callbacks that poll this.
    m_stop_source.request_stop();

    // 2. unblock a transfer that is sat waiting on the installer.
    //    the stream is taken out of the lock before use, the transport thread
    //    may be inside Push() with the stream mutex held.
    std::shared_ptr<Stream> source;
    {
        SCOPED_MUTEX(&m_mutex);
        source = m_source;
        condvarWakeAll(&m_idle_cv);
    }

    if (source) {
        source->Disable();
        source->SetInstallFinished();
    }

    // 3. tell the transport to stop accepting installs, this waits for any
    //    in-flight callback to return, which steps 1 and 2 just guaranteed.
    OnDisableInstallMode();
}

bool Menu::WaitForIdle() {
    for (;;) {
        if (IsCancelled()) {
            return false;
        }

        SCOPED_MUTEX(&m_mutex);
        // Connected means a stream is waiting to be picked up by the ui
        // thread, Progress means the installer still owns it. In both
        // cases the previous transfer is not done with yet.
        if (m_state != State::Connected && m_state != State::Progress) {
            return true;
        }

        if (GetToken().stop_requested()) {
            return false;
        }

        condvarWaitTimeout(&m_idle_cv, &m_mutex, WAIT_TIMEOUT);
    }
}

void Menu::Update(Controller* controller, TouchInfo* touch) {
    MenuBase::Update(controller, touch);

    if (!m_log.empty()) {
        m_list->OnUpdate(controller, touch, m_index, m_log.size(), [this](bool touch, auto i) {
            if (touch && m_index == i) {
                FireAction(Button::A);
            } else {
                App::PlaySoundEffect(SoundEffect::Focus);
                SetIndex(i);
            }
        });
    }

    std::shared_ptr<Stream> source;
    {
        SCOPED_MUTEX(&m_mutex);
        if (m_state != State::Connected) {
            return;
        }

        m_state = State::Progress;
        source = m_source;
    }

    // pushed outside of the lock, creating the progress box spawns a thread
    // whilst the transport thread is polling m_state.
    const std::string name = source->GetPath().s;
    App::Push<ui::ProgressBox>(0, "Installing "_i18n, source->GetPath(), [source](auto pbox) -> Result {
        // whatever happens, atomically mark finished so transport never blocks or errors in Push().
        ON_SCOPE_EXIT(source->SetInstallFinished());

        R_TRY(yati::InstallFromSource(pbox, source.get(), source->GetPath()));
        R_SUCCEED();
    }, [this, name](Result rc){
        if (m_log.size() >= MAX_LOG_ENTRIES) {
            m_log.erase(m_log.begin());
        }
        m_log.push_back(LogEntry{name, rc});

        if (R_SUCCEEDED(rc)) {
            m_ok_count++;
            App::Notify(i18n::Reorder("Installed ", name));
        } else {
            m_fail_count++;
            char buf[32];
            std::snprintf(buf, sizeof(buf), " (0x%X)", R_VALUE(rc));
            App::Notify("Install failed!"_i18n + " " + name + buf);
        }

        const auto count = static_cast<s64>(m_log.size());
        const auto page = m_list->GetPage();
        m_list->SetYoff(count > page ? (count - page) * m_list->GetMaxY() : 0.f);
        UpdateSummaryHeading();
        SetIndex(count - 1);

        SCOPED_MUTEX(&m_mutex);
        m_last_rc = rc;
        m_state = State::Done;
        condvarWakeAll(&m_idle_cv);
    }, ui::ProgressBoxOption::ScreenToggle);
}

void Menu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    if (m_log.empty()) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 36.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "Drag'n'Drop (NSP, XCI, NSZ, XCZ, MSP) to the install folder"_i18n.c_str());
        return;
    }

    constexpr float text_xoffset{15.f};
    m_list->Draw(vg, theme, m_log.size(), [this](auto* vg, auto* theme, const auto& v, auto i) {
        const auto& [x, y, w, h] = v;
        const auto& entry = m_log[i];

        auto text_id = ThemeEntryID_TEXT;
        if (m_index == i) {
            text_id = ThemeEntryID_TEXT_SELECTED;
            gfx::drawRectOutline(vg, theme, 4.f, v);
        } else if (i != static_cast<s64>(m_log.size()) - 1) {
            gfx::drawRect(vg, x, y + h, w, 1.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));
        }

        const bool ok = R_SUCCEEDED(entry.rc);
        const char* icon = ok ? "\uE14B" : "\uE140";
        const auto status_colour = theme->GetColour(ok ? ThemeEntryID_TEXT_SELECTED : ThemeEntryID_ERROR);
        gfx::drawText(vg, x + text_xoffset, y + h / 2.f, 22.f, icon, nullptr, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, status_colour);

        std::string right;
        if (ok) {
            right = "OK"_i18n;
        } else {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%s 0x%X", "Error"_i18n.c_str(), R_VALUE(entry.rc));
            right = buf;
        }

        float bounds[4]{};
        nvgFontSize(vg, 18.f);
        gfx::textBounds(vg, 0, 0, bounds, right.c_str());
        const float right_w = std::max(0.f, bounds[2] - bounds[0]);

        const float name_x = x + text_xoffset + 34.f;
        const float name_w = std::max(40.f, w - text_xoffset * 2.f - 34.f - right_w - 20.f);
        m_scroll_name.Draw(vg, m_index == i, name_x, y + h / 2.f, name_w, 20.f, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(text_id), entry.name);

        gfx::drawTextArgs(vg, x + w - text_xoffset, y + h / 2.f, 18.f, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE, ok ? theme->GetColour(ThemeEntryID_TEXT_INFO) : status_colour, "%s", right.c_str());
    });
}

void Menu::SetListRect(const Vec4& v) {
    m_list = std::make_unique<ui::List>(1, 8, m_pos, v);
}

void Menu::SetIndex(s64 index) {
    if (m_log.empty()) {
        m_index = 0;
        m_list->SetYoff(0);
        RemoveAction(Button::A);
        RemoveAction(Button::Y);
        return;
    }

    m_index = std::clamp<s64>(index, 0, static_cast<s64>(m_log.size()) - 1);
    if (!m_index) {
        m_list->SetYoff(0);
    }

    SetAction(Button::Y, Action{"Clear finished"_i18n, [this](){
        m_log.clear();
        m_ok_count = 0;
        m_fail_count = 0;
        UpdateSummaryHeading();
        SetIndex(0);
    }});

    if (R_FAILED(m_log[m_index].rc)) {
        SetAction(Button::A, Action{"Details"_i18n, [this](){
            App::PushErrorBox(m_log[m_index].rc, m_log[m_index].name);
        }});
    } else {
        RemoveAction(Button::A);
    }
}

void Menu::UpdateSummaryHeading() {
    if (m_ok_count || m_fail_count) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Installed: %zu  |  Failed: %zu"_i18n.c_str(), m_ok_count, m_fail_count);
        SetTitleSubHeading(buf);
    } else {
        SetTitleSubHeading("");
    }
}

bool Menu::OnInstallStart(const char* path) {
    log_write("[Menu::OnInstallStart] inside: %s\n", path);

    // wait for the previous transfer to fully complete. the state only leaves
    // Progress once the progress box has been destroyed, and that joins the
    // installer thread first, so by this point nothing can still be using the
    // old stream.
    if (!WaitForIdle()) {
        log_write("[Menu::OnInstallStart] cancelled whilst waiting\n");
        return false;
    }

    SCOPED_MUTEX(&m_mutex);

    if (GetToken().stop_requested()) {
        log_write("[Menu::OnInstallStart] rejecting, cancelled\n");
        return false;
    }

    m_source = std::make_shared<Stream>(path, GetToken());
    m_last_rc = 0;
    m_state = State::Connected;
    log_write("[Menu::OnInstallStart] exiting\n");

    return true;
}

bool Menu::OnInstallWrite(const void* buf, size_t size) {
    std::shared_ptr<Stream> source;
    {
        SCOPED_MUTEX(&m_mutex);
        source = m_source;
    }

    if (!source) {
        log_write("[Menu::OnInstallWrite] no source\n");
        return false;
    }

    return source->Push(buf, size);
}

Result Menu::OnInstallClose() {
    log_write("[Menu::OnInstallClose] inside\n");

    std::shared_ptr<Stream> source;
    {
        SCOPED_MUTEX(&m_mutex);
        source = m_source;
    }

    if (source) {
        // the file has been fully received, let the installer drain whatever
        // is still buffered.
        source->Disable();
    }

    // wait until the install has finished before returning so the next file
    // does not race with this one and failures can be reported synchronously.
    if (!WaitForIdle()) {
        R_THROW(Result_TransferCancelled);
    }

    SCOPED_MUTEX(&m_mutex);
    return m_last_rc;
}

} // namespace sphaira::ui::menu::stream
