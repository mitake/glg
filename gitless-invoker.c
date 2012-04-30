
#include <stdio.h>
#include <string.h>

#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "die.h"

static char *gitless_opts[] = {
	"gitless",
	NULL
};

static char *git_log_opts[] = {
	"git",
	"log",
	"-p",
	NULL
};

int main(int argc, char **argv)
{
	sigset_t sigset;
	int fds[2];

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds))
		die("socketpair() falied");

	switch (fork()) {
	case 0:
		/* gitless */

		setsid();

		close(0);
		dup(fds[0]);

		execvp(gitless_opts[0], gitless_opts);

		break;

	case -1:
		die("fork() failed");
		break;

	default:
		/* git log */

#if 0
		switch (fork()) {
		case -1:
			die("second fork() failed");
			break;

		case 0:
			/* do nothing */
			break;

		default:
			exit(0);
			break;
		}
#endif

#if 0
		bzero(&sigset, sizeof(sigset_t));

		if (sigprocmask(0, NULL, &sigset))
			die("sigprocmask() failed");
		if (sigaddset(&sigset, SIGINT))
			die("sigaddset() failed");
		if (sigprocmask(SIG_BLOCK, &sigset, NULL))
			die("sigprocmask() failed");
#endif
		close(1);
		dup(fds[1]);

		execvp(git_log_opts[0], git_log_opts);

		break;
	}

	return 0;
}
