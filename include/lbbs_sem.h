/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * lbbs_sem
 *   - portability wrappers of the semaphore calls behind the data pool locks
 *
 * Copyright (C) 2004-2026  Leaflet <leaflet@leafok.com>
 */

#ifndef _LBBS_SEM_H_
#define _LBBS_SEM_H_

#include <errno.h>
#include <time.h>
#include <unistd.h>

#define LBBS_SEM_POLL_INTERVAL_NS 10000000L // nanoseconds

#ifdef HAVE_SYSTEM_V

#include <sys/sem.h>

#if defined(HAVE_SEMTIMEDOP)

static inline int lbbs_semtimedop(int semid, struct sembuf *sops, size_t nsops, const struct timespec *timeout)
{
	return semtimedop(semid, sops, nsops, timeout);
}

#else

// semtimedop() is a Linux extension. The whole set of operations is retried with
// IPC_NOWAIT instead, so it still either takes effect as a unit or not at all.
static inline int lbbs_semtimedop(int semid, struct sembuf *sops, size_t nsops, const struct timespec *timeout)
{
	struct timespec deadline, now, nap;
	long long remain_ns;

	if (clock_gettime(CLOCK_REALTIME, &deadline) == -1)
	{
		return -1;
	}

	deadline.tv_sec += timeout->tv_sec;
	deadline.tv_nsec += timeout->tv_nsec;
	if (deadline.tv_nsec >= 1000000000L)
	{
		deadline.tv_nsec -= 1000000000L;
		deadline.tv_sec++;
	}

	for (;;)
	{
		struct sembuf nowait_ops[nsops];
		size_t i;

		for (i = 0; i < nsops; i++)
		{
			nowait_ops[i] = sops[i];
			nowait_ops[i].sem_flg = (short)(nowait_ops[i].sem_flg | IPC_NOWAIT);
		}

		if (semop(semid, nowait_ops, nsops) == 0)
		{
			return 0;
		}

		if (errno != EAGAIN && errno != EINTR)
		{
			return -1;
		}

		if (clock_gettime(CLOCK_REALTIME, &now) == -1)
		{
			return -1;
		}

		remain_ns = (long long)(deadline.tv_sec - now.tv_sec) * 1000000000LL + (deadline.tv_nsec - now.tv_nsec);
		if (remain_ns <= 0)
		{
			errno = ETIMEDOUT;
			return -1;
		}

		if (remain_ns > LBBS_SEM_POLL_INTERVAL_NS)
		{
			remain_ns = LBBS_SEM_POLL_INTERVAL_NS;
		}

		nap.tv_sec = (time_t)(remain_ns / 1000000000LL);
		nap.tv_nsec = (long)(remain_ns % 1000000000LL);

		if (nanosleep(&nap, NULL) == -1 && errno != EINTR)
		{
			return -1;
		}
	}
}

#endif // #if defined(HAVE_SEMTIMEDOP)

#else

#if defined(__APPLE__)
// The pool locks live in shared memory and are initialized with pshared = 1, which
// sem_init() rejects with ENOSYS on macOS, so only the System V back end can work here.
#error "macOS needs the System V semaphore back end, do not configure with --without-sysv"
#endif

#include <semaphore.h>

static inline int lbbs_sem_init(sem_t *sem, int pshared, unsigned int value)
{
	return sem_init(sem, pshared, value);
}

static inline int lbbs_sem_destroy(sem_t *sem)
{
	return sem_destroy(sem);
}

// Unlike sem_timedwait(), which expects a CLOCK_REALTIME deadline, the timeout is
// relative here so that both locking back ends take the caller's wait_sec as is.
static inline int lbbs_sem_timedwait(sem_t *sem, const struct timespec *rel_timeout)
{
	struct timespec deadline;

	if (clock_gettime(CLOCK_REALTIME, &deadline) == -1)
	{
		return -1;
	}

	deadline.tv_sec += rel_timeout->tv_sec;
	deadline.tv_nsec += rel_timeout->tv_nsec;
	if (deadline.tv_nsec >= 1000000000L)
	{
		deadline.tv_nsec -= 1000000000L;
		deadline.tv_sec++;
	}

	return sem_timedwait(sem, &deadline);
}

#endif // #ifdef HAVE_SYSTEM_V

#endif // #ifndef _LBBS_SEM_H_
