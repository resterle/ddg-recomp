// pthread.h for Windows builds: the part of POSIX threads the runtime uses, on Win32 (host/os_win.c).
// host/win/include comes first on the include path of Windows builds, so MinGW and MSVC builds use
// the same code (MinGW's winpthreads is not used).
#pragma once
#include <stddef.h>
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct { void *p; } pthread_mutex_t;        // an SRWLOCK
typedef struct { void *p; } pthread_cond_t;         // a CONDITION_VARIABLE
typedef void *pthread_t;                            // a thread HANDLE
typedef struct { size_t stack; } pthread_attr_t;
typedef int pthread_mutexattr_t, pthread_condattr_t;
#define PTHREAD_MUTEX_INITIALIZER {0}
#define PTHREAD_COND_INITIALIZER {0}

static inline int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) { (void)a; m->p = 0; return 0; }
static inline int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) { (void)a; c->p = 0; return 0; }
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
// ETIMEDOUT when CLOCK_REALTIME passed `abstime`
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);

static inline int pthread_attr_init(pthread_attr_t *a) { a->stack = 0; return 0; }
static inline int pthread_attr_setstacksize(pthread_attr_t *a, size_t n) { a->stack = n; return 0; }
static inline int pthread_attr_destroy(pthread_attr_t *a) { (void)a; return 0; }
int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg);

#ifdef __cplusplus
}
#endif
