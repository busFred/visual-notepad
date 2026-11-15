// For tests of a program that must not hold up its GUI thread.
#pragma once

#include <QtGlobal>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

// Milliseconds of processor time the calling thread has used. A stall of
// the GUI thread by the program is told from one by others: on a machine kept
// busy by other programs a thread can wait hundreds of milliseconds for a
// core whatever it does, which the clock counts and this does not, while a
// thread that is busy with the program's own work uses this time as fast as
// the clock runs. (Not for a thread that waits on others by itself.)
inline double threadCpuMs()
{
#ifdef Q_OS_WIN
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        return 0;
    const auto ticks = [](const FILETIME &t) { return double((quint64(t.dwHighDateTime) << 32) | t.dwLowDateTime); };
    return (ticks(kernel) + ticks(user)) / 10'000.0; // In 100 ns.
#else
    timespec now{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return now.tv_sec * 1000.0 + now.tv_nsec / 1e6;
#endif
}
