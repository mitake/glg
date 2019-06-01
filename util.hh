#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "commit.hh"

#include <string>

extern char dying_msg[1024];

extern struct commit *current;

#define die(fmt, arg...)				\
  do {							\
    int len;						\
    len = sprintf(dying_msg,				\
		  "line %d: fatal error, " fmt "\n",	\
		  __LINE__, ##arg);			\
    len += sprintf(dying_msg + len, "errno: %s\n",	\
		   strerror(errno));			\
    if (current)					\
      len += sprintf(dying_msg + len,			\
		     "current commit: %s\n",		\
		     current->commit_id);		\
    exit(1);						\
  } while (0)

#ifdef assert
#undef assert
#endif

#define assert(expr)					\
  ((expr) ?						\
   (void)0 :						\
   (sprintf(dying_msg, "assert: %s:%d: %s: "		\
	    "Asserting `%s' failed.\n",			\
	    __FILE__, __LINE__, __func__, #expr),	\
    exit(1)))
