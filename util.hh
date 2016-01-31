/*
 * glg - a specialized pager for git log
 *
 * Copyright (C) 2012 - 2016 Hitoshi Mitake <mitake.hitoshi@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "commit.hh"

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
