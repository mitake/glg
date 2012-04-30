
#ifndef DIE_H
#define DIE_H

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#define die(fmt, arg...)						\
	do {                                                            \
		fprintf(stderr, "line %d: fatal error, " fmt "\n",	\
			__LINE__, ##arg);				\
		fprintf(stderr, "errno: %s\n", strerror(errno));	\
		exit(1);						\
	} while (0)

#endif	/* DIE_H */
