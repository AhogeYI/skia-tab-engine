// The wake contract: wake() from any thread must run the handler on the
// thread that pumps the platform loop, never inline on the waking thread.

#include "tabengine/win32_platform.h"

#include "check.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <functional>
#include <thread>

namespace {

// Pumps the thread's message queue until `done` holds or `timeout_ms`
// elapses. Bounded waiting on the queue itself, never Sleep.
bool pump_until(const std::function<bool()>& done, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    MSG msg{};
    while (!done()) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return false;
        const DWORD slice = static_cast<DWORD>(deadline - now);
        const DWORD wait =
            MsgWaitForMultipleObjects(0, nullptr, FALSE, slice, QS_ALLINPUT);
        if (wait != WAIT_OBJECT_0) continue;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return true;
}

} // namespace

int main() {
    auto platform = tabengine::make_win32_platform();
    std::atomic<int> runs{0};
    std::atomic<DWORD> handler_thread{0};
    const DWORD pumping_thread = GetCurrentThreadId();
    platform->set_wake_handler([&] {
        ++runs;
        handler_thread = GetCurrentThreadId();
    });

    // A wake on the pumping thread itself is dispatched by the local pump.
    platform->wake();
    CHECK(pump_until([&] { return runs.load() > 0; }, 2000));
    CHECK(runs.load() == 1);
    CHECK(handler_thread.load() == pumping_thread);

    // A wake from a worker thread must not run the handler inline there;
    // the pump dispatches it on the pumping thread.
    const int before = runs.load();
    handler_thread = 0;
    std::thread worker([&] { platform->wake(); });
    worker.join();
    CHECK(runs.load() == before); // still not delivered
    CHECK(pump_until([&] { return runs.load() > before; }, 2000));
    CHECK(handler_thread.load() == pumping_thread);
    return 0;
}
