/*
 * gitless - a specialized pager for git-log
 *
 * Copyright (C) 2012 Hitoshi Mitake <h.mitake@gmail.com>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <errno.h>

#include <assert.h>

#include <regex.h>
#include <ncurses.h>

#include <stdbool.h>

#define GIT_LESS_DEBUG
#define DEBUG_FILE_NAME "/tmp/git-less-debug"

extern int errno;

#define die(fmt, arg...)						\
	do {                                                            \
		fprintf(stderr, "line %d: fatal error, " fmt "\n",	\
			__LINE__, ##arg);				\
		fprintf(stderr, "errno: %s\n", strerror(errno));	\
		exit(1);						\
	} while (0)

static void *xalloc(size_t size)
{
	void *ret;

	ret = calloc(sizeof(char), size);
	if (!ret)
		die("memory allocation failed");

	return ret;
}

static void *xrealloc(void *ptr, size_t size)
{
	void *ret;

	assert(size);
	ret = realloc(ptr, size);
	if (!ret)
		die("memory allocation failed");

	return ret;
}

static int ret_nl_index(char *s)
{
	int i;

	for (i = 0; s[i] != '\n'; i++);
	return i;
}

static int stdin_fd = 0, tty_fd;
#ifdef GIT_LESS_DEBUG
static int debug_fd;
#endif
static unsigned int row, col;
static int running = 1;
static int searching, visiting_root;

#define LINES_INIT_SIZE 128

enum {
	STATE_DEFAULT,
	STATE_INPUT_SEARCH_QUERY,
	STATE_SEARCHING_QUERY,
	STATE_INPUT_SEARCH_TYPE,
};
static int state = STATE_DEFAULT;

#define BOTTOM_MESSAGE_INIT_SIZE 32
static char *bottom_message;
static int bottom_message_size = BOTTOM_MESSAGE_INIT_SIZE;

#define MATCH_ARRAY_INIT_SIZE 32
static regmatch_t *match_array;
static int match_array_size = MATCH_ARRAY_INIT_SIZE;

#define bmprintf(fmt, arg...)						\
	do {                                                            \
		snprintf(bottom_message, bottom_message_size,		\
			fmt, ##arg);					\
	} while (0)

#ifdef GIT_LESS_DEBUG

static FILE* debug_file;

#define debug_printf(fmt, arg...)					\
	do {                                                            \
		fprintf(debug_file, fmt, ##arg);			\
	} while (0)

#else

#define debug_printf(fmt, arg...) do { } while (0)

#endif	/* GIT_LESS_DEBUG */

static void update_row_col(void)
{
	struct winsize size;

	bzero(&size, sizeof(struct winsize));
	ioctl(tty_fd, TIOCGWINSZ, (void *)&size);

	row = size.ws_row - 1;
	col = size.ws_col;

	if (bottom_message_size - 1 < col) {
		bottom_message_size = col + 1;
		bottom_message = xrealloc(bottom_message, bottom_message_size);
	}

	if (match_array_size < col) {
		match_array_size = col;
		match_array = xrealloc(match_array,
				match_array_size * sizeof(regmatch_t));
	}

	resizeterm(size.ws_row, size.ws_col);
}

/* static struct termios attr; */

#define COLORING_PLUS   1
#define COLORING_MINUS  2
#define COLORING_ATMARK 3
#define COLORING_COMMIT 4

static void init_tty(void)
{
	tty_fd = open("/dev/tty", O_RDONLY);
	if (tty_fd < 0)
		die("open()ing /dev/tty");

	initscr();

	cbreak();
	noecho();
	nonl();
	/* intrflush(stdscr, FALSE); */
	/* keypad(stdscr, TRUE); */

	start_color();

	init_pair(COLORING_PLUS, COLOR_GREEN, COLOR_BLACK);
	init_pair(COLORING_MINUS, COLOR_RED, COLOR_BLACK);
	init_pair(COLORING_ATMARK, COLOR_CYAN, COLOR_BLACK);
	init_pair(COLORING_COMMIT, COLOR_YELLOW, COLOR_BLACK);

	update_row_col();
}

static char *logbuf;
static int logbuf_size, logbuf_used;
#define LOGBUF_INIT_SIZE 1024

/* last_etx points last newline of previous commit */
static int last_etx;

struct commit {
	unsigned int *lines;	/* array of index of logbuf */
	int lines_size;

	int nr_lines, head_line;

	/*
	 * caution:
	 * prev means previous commit of the commit object,
	 * next means next commit of the commit object.
	 */
	struct commit *prev, *next;

	char *commit_id, *summary;
};

/* head: HEAD, root: root of the commit tree */
static struct commit *head, *root;
/* current: current displaying commit, tail: tail of the read commits */
static struct commit *current, *tail;
static struct commit *range_begin, *range_end;

static regex_t *re_compiled;

static void coloring(char ch, int on)
{
	int color = 0;

	switch (ch) {
	case '+':
		color = COLOR_PAIR(COLORING_PLUS);
		break;
	case '-':
		color = COLOR_PAIR(COLORING_MINUS);
		break;
	case '@':
		color = COLOR_PAIR(COLORING_ATMARK);
		break;
	case 'c':
		color = COLOR_PAIR(COLORING_COMMIT);
		break;

	default:
		return;

		break;
	}

	if (on)
		attron(color);
	else
		attroff(color);
}

static void update_terminal(void)
{
	int i, j;
	char *line;

	move(0, 0);
	clear();

	/* eliminating first new line */
	if (current != head && !current->head_line)
		current->head_line = 1;

	for (i = current->head_line;
	     i < current->head_line + row && i < current->nr_lines; i++) {
		char first_char;

		line = &logbuf[current->lines[i]];
		first_char = line[0];

		coloring(first_char, 1);

		if (state == STATE_SEARCHING_QUERY) {
			int ret, mi, nli = ret_nl_index(line);
			int rev = 0;

			line[nli] = '\0';
			ret = regexec(re_compiled, line,
				match_array_size, match_array, 0);
			line[nli] = '\n';

			if (ret)
				goto normal_print;

			for (mi = j = 0; j < col && line[j] != '\n'; j++) {
				if (j == match_array[mi].rm_so) {
					attron(A_REVERSE);
					rev = 1;
				} else if (j == match_array[mi].rm_eo) {
					attroff(A_REVERSE);
					rev = 0;

					mi++;
				}

				if (match_array[mi].rm_so
					== match_array[mi].rm_eo) {
					attroff(A_REVERSE);
					rev = 0;

					mi++;
				}

				addch(line[j]);
			}

			if (rev)
				attroff(A_REVERSE);
		} else {
		normal_print:
			for (j = 0; j < col && line[j] != '\n'; j++)
				addch(line[j]);

		}

		addch('\n');
		coloring(first_char, 0);
	}

	while (i++ < current->head_line + row)
		addch('\n');

	move(row, 0);
	attron(A_REVERSE);

	if (current->nr_lines <= current->head_line + row)
		printw("100%%");
	else
		printw("% .0f%%",
			(float)(current->head_line + row)
			/ current->nr_lines * 100.0);

	printw("   ");
	for (i = 0; i < 8; i++)
		printw("%c", current->commit_id[i]);
	printw(", ");
	printw("\"%s\"", current->summary);
	if (80 < strlen(current->summary))
		printw("...");

	if (strlen(bottom_message))
		printw(": %s", bottom_message);

	attroff(A_REVERSE);

	refresh();
}

static void signal_handler(int signum)
{
	switch (signum) {
	case SIGWINCH:
		update_row_col();
		update_terminal();
		break;

	case SIGINT:
		if (searching)
			searching = 0;
		else if (visiting_root)
			visiting_root = 0;
		else
			running = 0;
		break;

	default:
		die("unknown signal: %d", signum);
		break;
	}
}

static int init_sighandler(void)
{
	struct sigaction act;

	bzero(&act, sizeof(struct sigaction));
	act.sa_handler = signal_handler;
	sigaction(SIGWINCH, &act, NULL);
	sigaction(SIGINT, &act, NULL);

	return 0;
}

static int contain_visible_char(char *buf)
{
	int i, len = strlen(buf);

	for (i = 0; i < len; i++)
		if (buf[i] != ' ') return i;

	return -1;
}

static void init_commit(struct commit *c, int first_index, int last_index)
{
	int i, line_head;

	c->lines_size = LINES_INIT_SIZE;
	c->lines = xalloc(c->lines_size * sizeof(int));

	line_head = first_index;

	for (i = first_index; i < last_index; i++) {
		if (logbuf[i] != '\n')
			continue;

		c->lines[c->nr_lines++] = line_head;

		if (logbuf[line_head] == 'c') {
			c->commit_id = xalloc(40);
			memcpy(c->commit_id,
				&logbuf[line_head + 7 /* strlen("commit ") */],
				40);
		}

		line_head = i + 1;

		if (c->lines_size == c->nr_lines) {
			c->lines_size <<= 1;
			c->lines = xrealloc(c->lines,
					c->lines_size * sizeof(int));
		}
	}

	for (i = 0; i < c->nr_lines; i++) {
		int j, len, nli;
		char *line;

		line = &logbuf[c->lines[i]];

		if ((j = contain_visible_char(line)) < 1)
			continue;

		nli = ret_nl_index(&line[j]);
		line[j + nli] = '\0';
		len = strlen(&line[j]);
		c->summary = xalloc(len + 1);
		strcpy(c->summary, &line[j]);
		line[j + nli] = '\n';

		break;
	}

	assert(c->commit_id);
	assert(c->summary);
}

static int contain_etx(int begin, int end)
{
	int i;
	static int state = 0;

	for (i = begin; i < end; i++) {
		switch (state) {
		case 0:
			if (logbuf[i] == '\n')
				state = 1;
			break;
		case 1:
			if (logbuf[i] != '\n') {
				state = 0;

				if (logbuf[i] == 'c')
					return i - 1;
			}

			break;
		default:
			die("unknown state: %d", state);
			break;
		}
	}

	return -1;
}

static int read_end;

static void read_head(void)
{
	int prev_logbuf_used = 0;

	do {
		int rbyte;

		if (logbuf_used == logbuf_size) {
			logbuf_size <<= 1;
			logbuf = xrealloc(logbuf, logbuf_size);
		}

		rbyte = read(stdin_fd, &logbuf[logbuf_used],
			logbuf_size - logbuf_used);

		if (rbyte < 0) {
			if (errno == EINTR)
				continue;
			else
				die("read() failed");
		}

		if (!rbyte) {
			if (logbuf_used) {
				last_etx = logbuf_used;
				read_end = 1;

				break;
			} else
				exit(0); /* no input */
		}

		prev_logbuf_used = logbuf_used;
		logbuf_used += rbyte;
	} while ((last_etx =
			contain_etx(prev_logbuf_used, logbuf_used)) == -1);

	head = xalloc(sizeof(struct commit));
	init_commit(head, 0, last_etx);

	tail = current = head;
}

static void read_commit(void)
{
	int prev_last_etx = last_etx;
	int prev_logbuf_used, first_logbuf_used;
	struct commit *new_commit;

	if (read_end)
		return;

	if (last_etx + 1 < logbuf_used) {
		unsigned int tmp_etx;

		tmp_etx = contain_etx(last_etx + 1, logbuf_used);
		if (tmp_etx != -1) {
			last_etx = tmp_etx;
			goto skip_read;
		}
	}

	prev_logbuf_used = 0;
	first_logbuf_used = logbuf_used;

	do {
		int rbyte;

		if (logbuf_used == logbuf_size) {
			logbuf_size <<= 1;
			logbuf = xrealloc(logbuf, logbuf_size);
		}

		rbyte = read(stdin_fd, &logbuf[logbuf_used],
			logbuf_size - logbuf_used);

		if (errno == EINTR) {
			errno = 0;
			continue;
		}

		if (rbyte < 0)
			die("read() failed");

		if (!rbyte) {
			read_end = 1;
			last_etx = logbuf_used;

			break;
		}

		prev_logbuf_used = logbuf_used;
		logbuf_used += rbyte;
	} while ((last_etx =
			contain_etx(prev_logbuf_used, logbuf_used)) == -1);

skip_read:

	new_commit = xalloc(sizeof(struct commit));

	assert(!tail->prev);
	tail->prev = new_commit;
	new_commit->next = tail;

	tail = new_commit;

	init_commit(new_commit, prev_last_etx, last_etx);
}

static int show_prev_commit(char cmd)
{
	if (current == range_begin) {
		bmprintf("begin of range...");
		return 0;
	}

	if (!current->prev) {
		read_commit();

		if (!current->prev)
			return 0;
	}

	current = current->prev;
	current->head_line = 0;

	return 1;
}

static int show_next_commit(char cmd)
{
	if (current == range_end) {
		bmprintf("end of range...");
		return 1;
	}

	if (!current->next) {
		assert(current == head);
		return 0;
	}

	current = current->next;
	current->head_line = 0;

	return 1;
}

static int forward_line(char cmd)
{
	if (current->head_line + row < current->nr_lines) {
		current->head_line++;
		return 1;
	}

	return 0;
}

static int backward_line(char cmd)
{
	if (0 < current->head_line) {
		current->head_line--;
		return 1;
	}

	return 0;
}

static int goto_top(char cmd)
{
	if (!current->head_line)
		return 0;

	current->head_line = 0;
	return 1;
}

static int goto_bottom(char cmd)
{
	if (current->nr_lines < row)
		return 0;

	current->head_line = current->nr_lines - row;
	return 1;
}

static int forward_page(char cmd)
{
	if (current->nr_lines < current->head_line + row)
		return 0;

	current->head_line += row;
	return 1;
}

static int backward_page(char cmd)
{
	if (!current->head_line)
		return 0;

	current->head_line -= row;
	if (current->head_line < 0)
		current->head_line = 0;

	return 1;
}

static int show_root(char cmd)
{
	struct commit *p = current;

	if (root) {
		current = root;
		return 1;
	}

	visiting_root = 1;

	do {
		if (!current->prev)
			read_commit();

		if (!current->prev)
			break;

		current = current->prev;
	} while (visiting_root);

	if (!visiting_root) {
		current = p;
		bmprintf("stop visiting root commit");
	} else {
		visiting_root = 0;

		assert(!root);
		root = current;
	}

	return 1;
}

static int show_head(char cmd)
{
	if (current == head)
		return 0;

	current = head;
	return 1;
}

#define QUERY_SIZE 128
static char query[QUERY_SIZE + 1];
static int query_used;

static bool (*match_filter)(char *);

static bool match_filter_modified(char *line)
{
	return line[0] == '+' || line[0] == '-';
}

static bool match_filter_at(char *line)
{
	return line[0] == '@';
}

static bool match_filter_commit_message(char *line)
{
	/* TODO */
	return true;
}

static bool match_filter_default(char *line)
{
	return true;
}

static int match_line(char *line)
{
	if (!match_filter(line))
		return 0;

	if (!regexec(re_compiled, line, 0, NULL, REG_NOTEOL))
		return 1;

	return 0;
}

static int match_commit(struct commit *c, int direction, int prog)
{
	int i = c->head_line;
	int nli, result;
	char *line;

	if (prog) {
		if (direction) {
			if (c->nr_lines <= i - 1)
				return 0;
		} else {
			if (!i)
				return 0;
		}

		i += direction ? 1 : -1;
	}

	do {
		line = &logbuf[c->lines[i]];
		nli = ret_nl_index(line);

		line[nli] = '\0';
		result = match_line(line);
		line[nli] = '\n';

		if (result) {
			c->head_line = i;
			return 1;
		}

		i += direction ? 1 : -1;
	} while (direction ? i < c->nr_lines : 0 <= i);

	return 0;
}

static int do_search(int direction, int global, int prog)
{
	int result;
	struct commit *p;

	assert(!searching);
	searching = 1;

	result = match_commit(current, direction, prog);
	if (result || !global)
		goto no_match;

	if (direction) {
		if (current == range_begin)
			goto no_match;

		if (!current->prev)
			read_commit();

		if (current->prev)
			p = current->prev;
		else
			goto no_match;
	} else {
		if (current == range_end)
			goto no_match;

		if (current->next)
			p = current->next;
		else
			goto no_match;
	}

	do {
		if (direction)
			p->head_line = 0;
		else
			p->head_line = p->nr_lines - 1;

		result = match_commit(p, direction, prog);
		if (result)
			goto matched;

		if (p == range_begin || p == range_end)
			goto no_match;

		if (direction && !p->prev)
			read_commit();
	} while (searching && (direction ? (p = p->prev) : (p = p->next)));

	goto no_match;

matched:
	current = p;
no_match:
	searching = 0;

	return result;
}

static int current_direction, current_global;

#define update_query_bm()	do {					\
		bmprintf("%s %s search: %s",				\
			current_direction ? "forward" : "backward",	\
			current_global ? "global" : "local",		\
			query);						\
									\
	} while (0)

static struct {
	struct commit *commit;
	int head_line;
} orig_place;

static int _search(int key, int direction, int global)
{
	current_direction = direction;
	current_global = global;

	switch (state) {
	case STATE_DEFAULT:
	case STATE_SEARCHING_QUERY:
		query_used = 0;
		bzero(query, QUERY_SIZE);

		bmprintf("%s %s search: input type (/, m, a, l): ",
			current_direction ? "forward" : "backward",
			current_global ? "global" : "local");

		state = STATE_INPUT_SEARCH_TYPE;
		break;

	case STATE_INPUT_SEARCH_QUERY:
		if (query_used + 1 == QUERY_SIZE) {
			bmprintf("search query is too long!");
			state = STATE_DEFAULT;

			goto end;
		}

		if (key == 0xd /* FIXME: \n ?*/) {
			state = STATE_SEARCHING_QUERY;

			orig_place.commit = current;
			orig_place.head_line = current->head_line;
		} else {
			query[query_used++] = (char)key;
			update_query_bm();
		}
	end:
		break;

	default:
		die("invalid or unknown state: %d", state);
		break;
	}

	if (state == STATE_SEARCHING_QUERY) {
		if (re_compiled)
			regfree(re_compiled);
		else
			re_compiled = xalloc(sizeof(regex_t));
		regcomp(re_compiled, query, REG_ICASE);

		if (!do_search(direction, global, 0))
			bmprintf("not found: %s", query);
		else
			update_query_bm();
	}

	return 1;
}

static int search(int direction, int global)
{
	return _search(-1, direction, global);
}

static int search_global_forward(char cmd)
{
	return search(1, 1);
}

static int search_global_backward(char cmd)
{
	return search(0, 1);
}

static int search_local_forward(char cmd)
{
	return search(1, 0);
}

static int search_local_backward(char cmd)
{
	return search(0, 0);
}

static int search_progress(char cmd)
{
	if (state != STATE_SEARCHING_QUERY)
		return 0;

	if (!do_search(cmd == 'n' ?
			current_direction : !current_direction,
			current_global, 1))
		bmprintf("not found: %s", query);
	else
		update_query_bm();

	return 1;
}

static int input_query(char key)
{
	if (key == (char)0x7f) {
		/* backspace */
		if (!query_used)
			return 0;

		query[--query_used] = '\0';
		update_query_bm();

		return 1;
	} else if (key == (char)0x1b) {
		/* escape */
		if (re_compiled) {
			regfree(re_compiled);
			free(re_compiled);
			re_compiled = NULL;
		}

		query_used = 0;
		bzero(query, QUERY_SIZE);

		bzero(bottom_message, bottom_message_size);
		state = STATE_DEFAULT;
		return 1;
	}

	return _search(key, current_direction, current_global);
}

static int restore_orig_place(char cmd)
{
	if (!orig_place.commit)
		return 0;

	current = orig_place.commit;
	current->head_line = orig_place.head_line;

	bmprintf("restored  original place");

	return 1;
}

static int save_orig_place(char cmd)
{
	orig_place.commit = current;
	orig_place.head_line = current->head_line;

	bmprintf("saved current as original place");

	return 1;
}

static int nop(char cmd)
{
	return 0;
}

static int quit(char cmd)
{
	running = 0;
	return 0;
}

enum {
	RANGE_INIT,
	RANGE_BEGIN_SPECIFIED,
	RANGE_END_SPECIFIED,
	RANGE_SPECIFIED
};
static int range_state = RANGE_INIT;

static int specify_range(char cmd)
{
	int begin_set, end_set;

	begin_set = end_set = 0;

	switch (range_state) {
	case RANGE_INIT:
		if (cmd == '[') {
			range_begin = current;
			range_state = RANGE_BEGIN_SPECIFIED;

			begin_set = 1;
		} else {
			assert(cmd == ']');

			range_end = current;
			range_state = RANGE_BEGIN_SPECIFIED;

			end_set = 1;
		}

		break;
	case RANGE_BEGIN_SPECIFIED:
		if (cmd == '[') {
			range_begin = current;
			range_state = RANGE_BEGIN_SPECIFIED;

			begin_set = 1;
		} else {
			assert(cmd == ']');

			range_end = current;
			range_state = RANGE_SPECIFIED;

			end_set = 1;
		}

		break;
	case RANGE_END_SPECIFIED:
		if (cmd == '[') {
			range_begin = current;
			range_state = RANGE_SPECIFIED;

			begin_set = 1;
		} else {
			assert(cmd == ']');

			range_end = current;
			range_state = RANGE_END_SPECIFIED;

			end_set = 1;
		}

		break;
	case RANGE_SPECIFIED:
		if (cmd == '[') {
			range_begin = current;
			begin_set = 1;
		} else {
			assert(cmd == ']');
			range_end = current;
			end_set = 1;
		}

		break;
	default:
		fprintf(stderr, "invalid state: %d\n", range_state);
		exit(1);
		break;
	}

	if (range_state == RANGE_SPECIFIED) {
		bmprintf("range specified");
		return 1;
	}

	if (begin_set)
		bmprintf("begin of range specified");
	else if (end_set)
		bmprintf("end of range specified");
	else
		return 0;

	return 1;
}

static int clear_range(char cmd)
{
	range_begin = range_end = NULL;
	range_state = RANGE_INIT;

	bmprintf("range cleared");

	return 1;
}

struct key_cmd {
	char key;
	int (*op)(char);
};

static struct key_cmd valid_ops[] = {
	{ 'h', show_prev_commit },
	{ 'j', forward_line },
	{ 'k', backward_line },
	{ 'l', show_next_commit },
	{ 'q', quit },
	{ 'g', goto_top },
	{ 'G', goto_bottom },
	{ ' ', forward_page },
	{ 'J', forward_page },
	{ 'K', backward_page },
	{ 'H', show_root },
	{ 'L', show_head },
	{ '/', search_global_forward },
	{ '?', search_global_backward },
	{ '\\', search_local_forward },
	{ '!', search_local_backward },
	{ 'n', search_progress },
	{ 'p', search_progress },
	{ 'o', restore_orig_place },
	{ 's', save_orig_place },

	{ '[', specify_range },
	{ ']', specify_range },
	{ 'R', clear_range },

	{ '\0', NULL },
};

static int search_type_modified_line(char cmd)
{
	match_filter = match_filter_modified;
	state = STATE_INPUT_SEARCH_QUERY;
	return 1;
}

static int search_type_at_line(char cmd)
{
	match_filter = match_filter_at;
	state = STATE_INPUT_SEARCH_QUERY;
	return 1;
}

static int search_type_commit_message(char cmd)
{
	match_filter = match_filter_commit_message;
	state = STATE_INPUT_SEARCH_QUERY;
	return 1;
}

static int search_type_default(char cmd)
{
	match_filter = match_filter_default;
	state = STATE_INPUT_SEARCH_QUERY;
	return 1;
}

static int search_type_cancel(char cmd)
{
	match_filter = match_filter_default;
	state = STATE_DEFAULT;
	return 1;
}

static int search_type_invalid(char cmd)
{
	bmprintf("invalid search type: %c\n", cmd);
	state = STATE_DEFAULT;
	return 1;
}

static struct key_cmd search_type_ops[] = {
	{ 'm', search_type_modified_line },
	{ 'a', search_type_at_line },
	{ 'l', search_type_commit_message },
	{ '/', search_type_default },

	{ 0x1b, search_type_cancel },

	{ '\0', NULL },
};

static int (*ops_array[256])(char);
static int (*search_type_ops_array[256])(char);

static void exit_handler(void)
{
	addch('\n');

#ifdef GIT_LESS_DEBUG
	/* unlink(DEBUG_FILE_NAME); */
#endif

	endwin();
}

int main(void)
{
	int i;
	char cmd;

#ifdef GIT_LESS_DEBUG

	unlink(DEBUG_FILE_NAME);
	debug_fd = open(DEBUG_FILE_NAME, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
	if (debug_fd < 0)
		die("failed to open() file: %s for debugging", DEBUG_FILE_NAME);

	debug_file = fdopen(debug_fd, "w");
	if (!debug_file)
		die("failed fdopen() for debug_file");

#endif

	bottom_message = xalloc(bottom_message_size);
	match_array = xalloc(match_array_size * sizeof(regmatch_t));

	atexit(exit_handler);

	init_tty();
	init_sighandler();

	logbuf_size = LOGBUF_INIT_SIZE;
	logbuf = xalloc(logbuf_size);
	logbuf_used = 0;
	read_head();

	match_filter = match_filter_default;

	update_terminal();

	for (i = 0; i < 256; i++)
		ops_array[i] = nop;

	for (i = 0; valid_ops[i].key != '\0'; i++)
		ops_array[(int)valid_ops[i].key] = valid_ops[i].op;

	for (i = 0; i < 256; i++)
		search_type_ops_array[i] = search_type_invalid;

	for (i = 0; search_type_ops[i].key != '\0'; i++)
		search_type_ops_array[(int)search_type_ops[i].key]
			= search_type_ops[i].op;

	while (running) {
		int ret;

		ret = read(tty_fd, &cmd, 1);
		if (ret == -1 && errno == EINTR) {
			if (!running)
				break;

			errno = 0;
			continue;
		}

		if (ret != 1)
			die("reading key input failed");

		switch (state) {
		case STATE_INPUT_SEARCH_TYPE:
			ret = search_type_ops_array[(int)cmd](cmd);
			break;

		case STATE_INPUT_SEARCH_QUERY:
			ret = input_query(cmd);
			break;

		case STATE_SEARCHING_QUERY:
		case STATE_DEFAULT:
			ret = ops_array[(int)cmd](cmd);
			break;

		default:
			die("invalid state: %d\n", state);
			break;
		}

		if (ret)
			update_terminal();
	}

	return 0;
}
