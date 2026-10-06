#pragma once

#include "defines.hpp"
#include "utils/core.hpp"
#include <functional>
#include <atomic>

namespace sphaira::utils {

static inline Result CreateThread(Thread *t, ThreadFunc entry, void *arg, size_t stack_sz = 1024*128, int prio = 0x3B) {
    const auto core_mask = GetSafeCoreMask();
    R_UNLESS(core_mask, Result_CoreUnavailable);
    R_TRY(threadCreate(t, entry, arg, nullptr, stack_sz, prio, -2));
    const auto rc = svcSetThreadCoreMask(t->handle, -1, core_mask);
    if (R_FAILED(rc)) {
        threadClose(t);
        return rc;
    }
    R_SUCCEED();
}

struct Async final {
    using Callback = std::function<void(void)>;

    // core0=main, core1=audio, core2=servers (ftp,mtp,nxlink)
    Async(Callback&& callback) : m_callback{std::forward<Callback>(callback)} {
        m_running = true;

        if (R_FAILED(CreateThread(&m_thread, thread_func, &m_callback))) {
            m_running = false;
            return;
        }

        if (R_FAILED(threadStart(&m_thread))) {
            threadClose(&m_thread);
            m_running = false;
        }
    }

    ~Async() {
        WaitForExit();
    }

    // false if the thread could not be created or started.
    auto IsRunning() const -> bool {
        return m_running;
    }

    void WaitForExit() {
        if (m_running) {
            threadWaitForExit(&m_thread);
            threadClose(&m_thread);
            m_running = false;
        }
    }

private:
    static void thread_func(void* arg) {
        (*static_cast<Callback*>(arg))();
    }

private:
    Callback m_callback;
    Thread m_thread{};
    std::atomic_bool m_running{};
};

} // namespace sphaira::utils
