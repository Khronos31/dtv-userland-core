// SPDX-License-Identifier: GPL-2.0-only
// Private MSVC binding for the existing worker's nonthrowing pthread subset.
// The lane/queue/lifetime authority remains in control_workers.cpp.
#ifndef DTV_HOST_DAEMON_WINDOWS_PTHREAD_COMPAT_H
#define DTV_HOST_DAEMON_WINDOWS_PTHREAD_COMPAT_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cerrno>
#include <cstdlib>

using pthread_t = HANDLE;
using pthread_mutex_t = CRITICAL_SECTION;
using pthread_cond_t = CONDITION_VARIABLE;

inline int pthread_mutex_init(pthread_mutex_t* mutex, const void*) noexcept
{ return ::InitializeCriticalSectionEx(mutex, 0U, 0U) ? 0 : ENOMEM; }
inline int pthread_mutex_destroy(pthread_mutex_t* mutex) noexcept
{ ::DeleteCriticalSection(mutex); return 0; }
inline int pthread_mutex_lock(pthread_mutex_t* mutex) noexcept
{ ::EnterCriticalSection(mutex); return 0; }
inline int pthread_mutex_unlock(pthread_mutex_t* mutex) noexcept
{ ::LeaveCriticalSection(mutex); return 0; }
inline int pthread_cond_init(pthread_cond_t* condition, const void*) noexcept
{ ::InitializeConditionVariable(condition); return 0; }
inline int pthread_cond_destroy(pthread_cond_t*) noexcept { return 0; }
inline int pthread_cond_wait(pthread_cond_t* condition, pthread_mutex_t* mutex) noexcept
{ return ::SleepConditionVariableCS(condition, mutex, INFINITE) ? 0 : EINVAL; }
inline int pthread_cond_signal(pthread_cond_t* condition) noexcept
{ ::WakeConditionVariable(condition); return 0; }
inline int pthread_cond_broadcast(pthread_cond_t* condition) noexcept
{ ::WakeAllConditionVariable(condition); return 0; }

struct DtvHostDaemonThreadStart {
    void* (*run)(void*);
    void* argument;
};
inline DWORD WINAPI dtv_host_daemon_thread_start(void* opaque) noexcept
{
    const auto start = *static_cast<DtvHostDaemonThreadStart*>(opaque);
    std::free(opaque);
    (void)start.run(start.argument);
    return 0U;
}
inline int pthread_create(pthread_t* thread, const void*, void* (*run)(void*),
                          void* argument) noexcept
{
    auto* start = static_cast<DtvHostDaemonThreadStart*>(
        std::malloc(sizeof(DtvHostDaemonThreadStart)));
    if (start == nullptr) return ENOMEM;
    start->run = run;
    start->argument = argument;
    const HANDLE created = ::CreateThread(nullptr, 0U, dtv_host_daemon_thread_start,
                                           start, 0U, nullptr);
    if (created == nullptr) {
        std::free(start);
        return EAGAIN;
    }
    *thread = created;
    return 0;
}
inline int pthread_join(pthread_t thread, void**) noexcept
{
    if (::WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0) return EINVAL;
    return ::CloseHandle(thread) ? 0 : EINVAL;
}
#endif
