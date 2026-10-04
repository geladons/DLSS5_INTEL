// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m11d - death witnesses (see crash_witness.h). The daemon's historical
// crashes were 0xc0000409 fail-fasts (CRT abort / terminate / invalid-param /
// pure-virtual), which bypass SEH and leave no trace. These handlers stamp
// the failure class into the log before dying so the next silent death names
// itself. _set_purecall_handler / _set_invalid_parameter_handler are
// process-wide.
// ============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>

#include "crash_witness.h"

static void nr_terminate_handler() {
    std::fprintf(stderr, "[m11d] FATAL: std::terminate called (unhandled exception or noexcept violation)\n");
    std::abort();
}
static void nr_purecall_handler() {
    std::fprintf(stderr, "[m11d] FATAL: pure virtual function call\n");
    std::abort();
}
static void nr_invalid_param_handler(const wchar_t* expr, const wchar_t* func,
                                     const wchar_t* file, unsigned int line,
                                     uintptr_t) {
    std::fprintf(stderr, "[m11d] FATAL: invalid CRT parameter %S %S %S:%u\n",
                 expr ? expr : L"-", func ? func : L"-", file ? file : L"-", line);
    std::abort();
}
static void nr_abort_signal(int) {
    std::fprintf(stderr, "[m11d] FATAL: abort()\n");
}
// SEH faults on threads the engine_seh wrappers don't cover (main loop, CRT
// internals, driver worker threads) used to die silently to WER.
static LONG WINAPI nr_unhandled_filter(EXCEPTION_POINTERS* ep) {
    std::fprintf(stderr, "[m11d] FATAL: unhandled SEH 0x%08lx at %p\n",
                 ep && ep->ExceptionRecord ?
                    (unsigned long)ep->ExceptionRecord->ExceptionCode : 0ul,
                 ep && ep->ExceptionRecord ?
                    ep->ExceptionRecord->ExceptionAddress : nullptr);
    return EXCEPTION_EXECUTE_HANDLER;   // let WER / exit proceed
}

void install_death_witnesses() {
    std::set_terminate(nr_terminate_handler);
    _set_purecall_handler(nr_purecall_handler);
    _set_invalid_parameter_handler(nr_invalid_param_handler);
    std::signal(SIGABRT, nr_abort_signal);
    SetUnhandledExceptionFilter(nr_unhandled_filter);
}
