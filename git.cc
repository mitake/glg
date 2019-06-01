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

