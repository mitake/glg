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

#include <unistd.h>

#include "util.hh"

void launch_git_log(int inputfd)
{
  int pipefds[2];
  if (pipe(pipefds))
    die("pipe() failed\n");

  pid_t pid = fork();
  switch (pid) {
  case 0:
    setsid();	/* for handling ^c correctly */

    close(1);
    dup(pipefds[1]);
    close(pipefds[0]);

    if (execlp("git", "git", "log", "--pretty=format:%H", NULL))
      die("execlp() failed\n");

    break;
  case -1:
    die("fork failed\n");
    break;
  default:
    close(pipefds[1]);
    close(inputfd);
    dup(pipefds[0]); /* connect git log to stdin */
    break;
  }
}

