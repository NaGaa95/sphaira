#pragma once

#include <switch.h>
#include <functional>

namespace sphaira::libhaze {

bool Init();
bool IsInit();
void Exit();

using OnInstallStart = std::function<bool(const char* path)>;
using OnInstallWrite = std::function<bool(const void* buf, size_t size)>;
using OnInstallClose = std::function<Result()>;

void InitInstallMode(const OnInstallStart& on_start, const OnInstallWrite& on_write, const OnInstallClose& on_close);

// false once install mode has been (or is being) torn down. polled by the
// install callbacks so that they never block indefinitely.
bool IsInstallModeEnabled();

// stops accepting installs and waits for every in-flight install callback to
// return. never call this whilst holding a lock that a callback may need.
void DisableInstallMode();

} // namespace sphaira::libhaze
