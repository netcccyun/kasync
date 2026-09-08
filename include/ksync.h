#ifndef KSYNC_H_99
#define KSYNC_H_99
#ifndef _WIN32
#include <pthread.h>
#include <errno.h>
#include <sched.h>
#include <sys/time.h>
#endif
#include <stdlib.h>
#include <string.h>
#include "kforwin32.h"
#include "katom.h"
#include "kfeature.h"
#include "kmalloc.h"
KBEGIN_DECLS
typedef pthread_mutex_t kmutex;
#define kmutex_init pthread_mutex_init
#define kmutex_lock pthread_mutex_lock
#define kmutex_unlock pthread_mutex_unlock
#define kmutex_destroy pthread_mutex_destroy

#ifdef _WIN32
typedef void kcond;
#else
typedef struct {
	bool ev;
	bool auto_reset;
	kmutex  mutex;
	pthread_cond_t cond;
} kcond;
#endif
INLINE kcond *kcond_init(bool auto_reset) {
#ifdef _WIN32
	return  CreateEvent(NULL, (auto_reset ? FALSE : TRUE), FALSE, NULL);
#else
	kcond *h = (kcond *)xmalloc(sizeof(kcond));
	memset(h, 0, sizeof(kcond));
	kmutex_init(&h->mutex, NULL);
	pthread_cond_init(&h->cond, NULL);
	h->auto_reset = auto_reset;
	h->ev = false;
	return h;
#endif
}
INLINE void kcond_reset(kcond* cond)
{
#ifdef _WIN32
	ResetEvent(cond);
#else
	kmutex_lock(&cond->mutex);
	cond->ev = false;
	kmutex_unlock(&cond->mutex);
#endif
}
INLINE void kcond_wait(kcond *cond)
{
#ifdef _WIN32
	WaitForSingleObject(cond, INFINITE);
#else
	kmutex_lock(&cond->mutex);
	while (!cond->ev) {
		pthread_cond_wait(&cond->cond, &cond->mutex);
	}
	if (cond->auto_reset) {
		cond->ev = false;
	}
	kmutex_unlock(&cond->mutex);
#endif
}
INLINE bool kcond_try_wait(kcond* cond, int msec) {
#ifdef _WIN32
	return WaitForSingleObject(cond, msec) == WAIT_OBJECT_0;
#else
	kmutex_lock(&cond->mutex);
	int ret = 0;
	if (!cond->ev && msec < 0) {
		while (!cond->ev) {
			pthread_cond_wait(&cond->cond, &cond->mutex);
		}
	} else if (!cond->ev && msec > 0) {
		struct timeval now;
		struct timespec deadline;
		gettimeofday(&now, NULL);
		deadline.tv_sec = now.tv_sec + msec / 1000;
		deadline.tv_nsec = now.tv_usec * 1000 + (msec % 1000) * 1000000;
		if (deadline.tv_nsec >= 1000000000) {
			deadline.tv_sec++;
			deadline.tv_nsec -= 1000000000;
		}
		while (!cond->ev && ret == 0) {
			ret = pthread_cond_timedwait(&cond->cond, &cond->mutex, &deadline);
		}
	}
	bool signaled = cond->ev;
	if (signaled && cond->auto_reset) {
		cond->ev = false;
	}
	kmutex_unlock(&cond->mutex);
	return signaled;
#endif
}
INLINE void kcond_notice(kcond *cond)
{
#ifdef _WIN32
	SetEvent(cond);
#else
	kmutex_lock(&cond->mutex);
	cond->ev = true;
	if (cond->auto_reset) {
		pthread_cond_signal(&cond->cond);
	} else {
		pthread_cond_broadcast(&cond->cond);
	}
	kmutex_unlock(&cond->mutex);
#endif
}
INLINE void kcond_destroy(kcond *cond)
{
#ifdef _WIN32
	CloseHandle(cond);
#else
	pthread_mutex_destroy(&cond->mutex);
	pthread_cond_destroy(&cond->cond);
	xfree(cond);
#endif
}
INLINE void kgl_pause()
{
#ifdef _WIN32
	YieldProcessor();
#elif defined(__i386__) || defined(__x86_64__)
	__asm__ __volatile__("pause");
#elif defined(__arm__) || defined(__aarch64__)
	__asm__ __volatile__("yield");
#else
	sched_yield();
#endif
}

//读优先
typedef volatile int32_t krw_mutex;
INLINE void krw_mutex_init(krw_mutex *mutex)
{
	*mutex = 0;
}
INLINE void krw_mutex_rlock(krw_mutex *mutex)
{
	int32_t x;
	for (;;) {
		x = *mutex;
		/* write lock is held */
		if (x < 0) {
			kgl_pause();
			continue;
		}
		if (katom_cas((void *)mutex, x, x + 1)) {
			//lock success
			break;
		}
	}
	return;
}
INLINE void krw_mutex_wlock(krw_mutex *mutex)
{
	for (;;) {
		/* write lock is held */
		if (*mutex != 0) {
			kgl_pause();
			continue;
		}
		if (katom_cas((void *)mutex, 0, -1)) {
			//lock success
			break;
		}
	}
}
INLINE void krw_mutex_wunlock(krw_mutex *mutex)
{
	katom_inc((void *)mutex);
}
INLINE void krw_mutex_runlock(krw_mutex *mutex)
{
	katom_dec((void *)mutex);
}

//写优先
typedef struct {
	krw_mutex rw;
	krw_mutex try_write;
} kwr_mutex;

INLINE void kwr_mutex_init(kwr_mutex *mutex)
{
	krw_mutex_init(&mutex->rw);
	krw_mutex_init(&mutex->try_write);
}
INLINE void kwr_mutex_rlock(kwr_mutex *mutex, bool high_priority)
{
	if (high_priority) {
		krw_mutex_rlock(&mutex->rw);
		return;
	}
	while (mutex->try_write > 0) {
		kgl_pause();
	}
	krw_mutex_rlock(&mutex->rw);
}
INLINE void kwr_mutex_wlock(kwr_mutex *mutex)
{
	for (;;) {
		if (mutex->try_write != 0) {
			kgl_pause();
			continue;
		}
		if (katom_cas((void *)&mutex->try_write, 0, 1)) {
			//try_write  success
			break;
		}
	}
	krw_mutex_wlock(&mutex->rw);
}
INLINE void kwr_mutex_wunlock(kwr_mutex *mutex)
{
	krw_mutex_wunlock(&mutex->rw);
	katom_set((void *)&mutex->try_write, 0);
}
INLINE void kwr_mutex_runlock(kwr_mutex *mutex)
{
	krw_mutex_runlock(&mutex->rw);
}
KEND_DECLS
#endif
