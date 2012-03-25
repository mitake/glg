#ifndef GITLESS_TTY_H
#define GITLESS_TTY_H

enum {
	TTY_STATE_INIT,
	TTY_STATE_WAIT_PAREN,
	TTY_STATE_BRANCH,

	TTY_STATE_SGR_0,
	TTY_STATE_SGR_1,
	TTY_STATE_SGR_2,

	TTY_STATE_FIN
};

#define ESC 033

int tty_state_trans(int *next_state, char *input, int end_i);

#endif	/* GITLESS_TTY_H */
