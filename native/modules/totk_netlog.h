// SPDX-License-Identifier: GPL-3.0-or-later

// Debug-only log stream: one UDP datagram per line to a listener on the LAN, for testing on a
// device without adb. It carries the module's own diagnostics and, on Android, the tail of the
// emulator's log file (eden_log.txt, the DSMod runtime's messages included). Compiled in only
// when TOTK_NETLOG_HOST is defined (a CMake option off by default); release builds have none of
// it.

#pragma once

#include <string_view>

namespace TotkNetLog {

/// Start the sender (and the log tail on Android). Safe to call more than once.
void Start();
/// Send what is queued, then stop and join the worker threads. Call before the library can be
/// unloaded (a refused Create, Destroy).
void Stop();
/// Queue one line (printf style). Cheap and non-blocking; dropped when the queue is full.
void Log(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
bool Enabled();

} // namespace TotkNetLog
