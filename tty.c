
#include "tty.h"
#include "misc.h"

#include <string.h>
#include <assert.h>

/* http://ascii-table.com/ansi-escape-sequences.php */

/* tty_state_trans(): only supports bold and color */
int tty_state_trans(int *next_state, char *input, int end_i)
{
	int i, state;

	state = *next_state;

	puts_exesc(input);
	exit(0);
	for (i = 0; (i < end_i) && (state != TTY_STATE_FIN); i++) {
		switch(state) {
		case TTY_STATE_INIT:
			assert(input[i] == ESC);
			state = TTY_STATE_WAIT_PAREN;
			break;

		case TTY_STATE_WAIT_PAREN:
			assert(input[i] == '[');
			state = TTY_STATE_BRANCH;
			break;

		case TTY_STATE_BRANCH:
			if (strchr("0123457689", input[i]))
				state = TTY_STATE_SGR_0;
			else if (input[i] == 'm')
				state = TTY_STATE_FIN;
			else
				state = TTY_STATE_FIN;
				/* die("unknown sequence: %c", input[i]); */
			break;

		case TTY_STATE_SGR_0:
			if (input[i] == 'm')
				state = TTY_STATE_FIN;
			else if (strchr("0123456789", input[i]))
				state = TTY_STATE_SGR_1;
			else
				die("unknown char: %c", input[i]);
			break;

		case TTY_STATE_SGR_1:
			if (input[i] == 'm')
				state = TTY_STATE_FIN;
			else if (strchr("0123456789", input[i]))
				state = TTY_STATE_SGR_2;
			else
				die("unknown char: %c", input[i]);
			break;

		case TTY_STATE_SGR_2:
			if (input[i] == 'm')
				state = TTY_STATE_FIN;
			else
				die("unknown char: %c (%x)", input[i], input[i]);
			break;

		default:
			die("unknown state: %d", state);
		}
	}

	if (state == TTY_STATE_FIN)
		state = TTY_STATE_INIT;

	*next_state = state;

	return i;
}
