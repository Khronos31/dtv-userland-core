// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_WINDOWS_PTHREAD_COMPAT_H
#define PX4_USERLAND_WINDOWS_PTHREAD_COMPAT_H

#include <cerrno>
#include <memory>
#include <new>
#include <process.h>
#include <windows.h>

using pthread_t = HANDLE;
using pthread_mutex_t = CRITICAL_SECTION;
using pthread_cond_t = CONDITION_VARIABLE;

inline int pthread_mutex_init(pthread_mutex_t* mutex, const void*) noexcept
{
    return InitializeCriticalSectionEx(mutex, 0U, 0U) ? 0 : ENOMEM;
}

inline int pthread_mutex_destroy(pthread_mutex_t* mutex) noexcept
{
    DeleteCriticalSection(mutex);
    return 0;
}

inline int pthread_mutex_lock(pthread_mutex_t* mutex) noexcept
{
    EnterCriticalSection(mutex);
    return 0;
}

inline int pthread_mutex_unlock(pthread_mutex_t* mutex) noexcept
{
    LeaveCriticalSection(mutex);
    return 0;
}

inline int pthread_cond_init(pthread_cond_t* condition, const void*) noexcept
{
    InitializeConditionVariable(condition);
    return 0;
}

inline int pthread_cond_destroy(pthread_cond_t*) noexcept
{
    return 0;
}

inline int pthread_cond_wait(pthread_cond_t* condition,
                             pthread_mutex_t* mutex) noexcept
{
    return SleepConditionVariableCS(condition, mutex, INFINITE) ? 0 : EINVAL;
}

inline int pthread_cond_signal(pthread_cond_t* condition) noexcept
{
    WakeConditionVariable(condition);
    return 0;
}

inline int pthread_cond_broadcast(pthread_cond_t* condition) noexcept
{
    WakeAllConditionVariable(condition);
    return 0;
}

struct PthreadStart final {
    void* (*routine)(void*) = nullptr;
    void* argument = nullptr;
};

inline unsigned __stdcall pthread_start(void* context) noexcept
{
    std::unique_ptr<PthreadStart> start(static_cast<PthreadStart*>(context));
    (void)start->routine(start->argument);
    return 0U;
}

inline int pthread_create(pthread_t* thread, const void*,
                          void* (*routine)(void*), void* argument) noexcept
{
    std::unique_ptr<PthreadStart> start(new (std::nothrow) PthreadStart{routine, argument});
    if (!start) return ENOMEM;
    const uintptr_t handle = _beginthreadex(nullptr, 0U, &pthread_start,
                                            start.get(), 0U, nullptr);
    if (handle == 0U) return EAGAIN;
    *thread = reinterpret_cast<HANDLE>(handle);
    (void)start.release();
    return 0;
}

inline int pthread_join(pthread_t thread, void**) noexcept
{
    if (WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0) return EINVAL;
    return CloseHandle(thread) ? 0 : EINVAL;
}

#endif
