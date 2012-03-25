#ifndef GITLESS_MISC_H
#define GITLESS_MISC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

extern int errno;

#define die(fmt, arg...)						\
	do {                                                            \
		fprintf(stderr, "file: %s, line %d: fatal error, "	\
			fmt "\n",					\
			__FILE__, __LINE__, ##arg);			\
		fprintf(stderr, "errno: %s\n", strerror(errno));	\
		exit(1);						\
	} while (0)

#define puts_exesc(string)			\
	do {					\
		int i, len;			\
						\
		len = strlen(string);				\
		for (i = 0; i < len; i++) {			\
			if (string[i] != 033)			\
				putchar(string[i]);	\
			else					\
				printf("\\033");		\
		}						\
	} while (0)

#endif	/* GITLESS_MISC_H */
