/* glibc's <sys/queue.h> plus the one macro it lacks that the NetBSD code uses */
#include_next <sys/queue.h>
#ifndef TAILQ_FOREACH_SAFE
#define TAILQ_FOREACH_SAFE(var, head, field, next)			\
	for ((var) = TAILQ_FIRST((head));				\
	    (var) && ((next) = TAILQ_NEXT((var), field), 1);		\
	    (var) = (next))
#endif
