/**
 * @file    sim_mutex.h
 * @brief   Cross-platform mutex wrapper for simulator runtime protection.
 * @date    2026-06
 */
#ifndef SIM_MUTEX_H
#define SIM_MUTEX_H

#ifdef _WIN32
#include <windows.h>

typedef CRITICAL_SECTION SimMutex;

#define sim_mutex_init(m)    InitializeCriticalSection(m)
#define sim_mutex_destroy(m) DeleteCriticalSection(m)
#define sim_mutex_lock(m)    EnterCriticalSection(m)
#define sim_mutex_unlock(m)  LeaveCriticalSection(m)

#else
#include <pthread.h>

typedef pthread_mutex_t SimMutex;

#define sim_mutex_init(m)    pthread_mutex_init((m), NULL)
#define sim_mutex_destroy(m) pthread_mutex_destroy(m)
#define sim_mutex_lock(m)    pthread_mutex_lock(m)
#define sim_mutex_unlock(m)  pthread_mutex_unlock(m)

#endif

#endif /* SIM_MUTEX_H */