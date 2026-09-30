/* <sys/queue.h>: the host's, plus TAILQ_FOREACH_SAFE where it lacks it (glibc);
 * NetBSD's own copy (netbsd/sys/sys/queue.h, unmodified) where the host has
 * none (musl). */
#ifndef COMPAT_SYS_QUEUE_H
#define COMPAT_SYS_QUEUE_H
#if defined(__has_include_next)
#if __has_include_next(<sys/queue.h>)
#define COMPAT_HOST_QUEUE_H 1
#endif
#endif
#if COMPAT_HOST_QUEUE_H
#include_next <sys/queue.h>
#else
#include "../../netbsd/sys/sys/queue.h"
#endif
#ifndef TAILQ_FOREACH_SAFE
#define TAILQ_FOREACH_SAFE(var, head, field, next)			\
	for ((var) = TAILQ_FIRST((head));				\
	    (var) && ((next) = TAILQ_NEXT((var), field), 1);		\
	    (var) = (next))
#endif
#endif
