#pragma once

// Test assertions must fail through stderr and an exit code. The MSVC CRT
// assert dialog blocks unattended CTest runs and hides the failure from
// logs, so every test process also routes CRT reports, abort messages,
// unhandled-exception termination and Windows crash dialogs to stderr.
#include <cstdio>
#include <cstdlib>

#if defined(_MSC_VER)
#include <crtdbg.h>
#endif
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
#include <exception>
#define TABENGINE_TEST_TERMINATE_HANDLER 1
#endif

#if defined(_WIN32) && !defined(TABENGINE_TEST_NO_WINDOWS_HELPERS)
// Declared here instead of including <windows.h>: this header is pulled in
// by tests that include <winsock2.h>, and a stray windows.h would drag in
// winsock 1 first and break them. kernel32 is linked by default.
extern "C" __declspec(dllimport) unsigned int __stdcall SetErrorMode(unsigned int mode);
namespace test_support_detail {
inline constexpr unsigned int kSemFailCriticalErrors = 0x0001;
inline constexpr unsigned int kSemNoGpFaultErrorBox = 0x0002;
inline constexpr unsigned int kSemNoOpenFileErrorBox = 0x8000;
} // namespace test_support_detail
#endif

namespace test_support_detail {

struct QuietDialogs {
    QuietDialogs() {
#if defined(_MSC_VER)
        // Debug-STL and CRT assertions: write the report to stderr instead
        // of popping a modal dialog, and make abort() silent.
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        _set_error_mode(_OUT_TO_STDERR);
#endif
#if defined(_WIN32) && !defined(TABENGINE_TEST_NO_WINDOWS_HELPERS)
        // No WER dialog on crashes; the exit code carries the failure.
        ::SetErrorMode(kSemFailCriticalErrors | kSemNoGpFaultErrorBox |
                       kSemNoOpenFileErrorBox);
#endif
#if defined(TABENGINE_TEST_TERMINATE_HANDLER)
        std::set_terminate([] {
            std::fflush(stdout);
            if (std::current_exception()) {
                try {
                    std::rethrow_exception(std::current_exception());
                } catch (const std::exception& error) {
                    std::fprintf(stderr, "TERMINATE: unhandled exception: %s\n",
                                 error.what());
                } catch (...) {
                    std::fprintf(stderr, "TERMINATE: unhandled non-standard exception\n");
                }
            } else {
                std::fprintf(stderr, "TERMINATE: terminate called without an exception\n");
            }
            std::fflush(stderr);
            std::abort();
        });
#endif
    }
};

inline const QuietDialogs kQuietDialogs;

} // namespace test_support_detail

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,      \
                         #condition);                                          \
            std::fflush(stderr);                                               \
            std::exit(2);                                                      \
        }                                                                      \
    } while (false)
