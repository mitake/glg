/*
 * gitless - a specialized pager for git log
 *
 * Copyright (C) 2012 - 2013 Hitoshi Mitake <h.mitake@gmail.com>
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
#include <sys/wait.h>

#include <regex.h>
#include <ncurses.h>

#include <stdbool.h>

#define GIT_LESS_DEBUG
#define DEBUG_FILE_NAME "/tmp/git-less-debug"

extern int errno;

static int running = 1;

static char dying_msg[1024];
#define die(fmt, arg...)						\
	do {								\
		int ret;						\
		ret = sprintf(dying_msg,				\
				"line %d: fatal error, " fmt "\n",	\
				__LINE__, ##arg);			\
		sprintf(dying_msg + ret, "errno: %s\n",			\
			strerror(errno));				\
		exit(1);						\
	} while (0)

#ifdef assert
#undef assert
#endif

#define assert(expr)							\
	((expr) ?							\
	(void)0 :							\
	(sprintf(dying_msg, "assert: %s:%d: %s: "			\
		"Asserting `%s' failed.\n",				\
		__FILE__, __LINE__, __func__, #expr),			\
		exit(1)))

#ifdef GIT_LESS_DEBUG
static int debug_fd;

#define dbgprintf(fmt, arg...)						\
	do {								\
		dprintf(debug_fd, "line %d: " fmt "\n",			\
			__LINE__, ##arg);				\
	} while (0)
#else

#define dbgprintf(fmt, arg...) do { } while (0)

#endif

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

	for (i = 0; s[i] != '\n'; i++); return i;
}

static int stdin_fd = 0, tty_fd;
static unsigned int row, col;
static int searching, visiting_root;

static char prev_cmd;		/* BIG FIXME: fatally ugly... */

#define LINES_INIT_SIZE 128

enum {
	STATE_DEFAULT,
	STATE_INPUT_SEARCH_QUERY,
	STATE_SEARCHING_QUERY,
	STATE_INPUT_SEARCH_FILTER,
	STATE_INPUT_SEARCH_FILTER2,
	STATE_INPUT_SEARCH_DIRECTION,
	STATE_LAUNCH_GIT_COMMAND,
	STATE_READ_BRANCHNAME_FOR_CHECKOUT,
	STATE_SHOW_CHANGED_FILES,
	STATE_HELP,
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

enum commit_cached_state {
	CACHED_STATE_PURGED,
	CACHED_STATE_FILLED,
};

struct commit_cached {
	enum commit_cached_state state;

	char *text;
	int text_size;

	char **lines;
	int nr_lines, lines_size;

	char **file_list;
	int nr_file_list, file_list_size;
};

struct commit {
	struct commit_cached cached;

	int head_line;

	/*
	 * caution:
	 * prev means previous commit of the commit object,
	 * next means next commit of the commit object.
	 */
	struct commit *prev, *next;

	/* size_next points next commit with smaller text size */
	struct commit *size_next;
	bool size_order_initialized;

	char *commit_id, *summary;
};

#define raw_get_cached(c) (&c->cached) /* simply get pointer */

static void init_commit_lines(struct commit *c)
{
	struct commit_cached *cached = raw_get_cached(c);
	char *text = cached->text;
	int text_size = cached->text_size;

	cached->lines_size = LINES_INIT_SIZE;
	cached->lines = xalloc(cached->lines_size * sizeof(char *));

	char *line_head = text;
	for (int i = 0; i < text_size; i++) {
		if (text[i] != '\n')
			continue;

		cached->lines[cached->nr_lines++] = line_head;

		if (line_head[0] == 'c') {
			c->commit_id = xalloc(41);
			memcpy(c->commit_id, line_head + 7 /* strlen("commit ") */, 40);
		}

		line_head = &text[i + 1];

		if (cached->lines_size == cached->nr_lines) {
			cached->lines_size <<= 1;
			cached->lines = xrealloc(cached->lines,
					cached->lines_size * sizeof(char *));
		}
	}

	if (c->summary)
		return;

	int contain_visible_char(char *buf)
	{
		int len = strlen(buf);

		for (int i = 0; i < len; i++)
			if (buf[i] != ' ') return i;

		return -1;
	}

	for (int i = 0; i < cached->nr_lines; i++) {
		int j, len, nli;
		char *line;

		line = cached->lines[i];

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

	for (int i = 0; i < cached->nr_lines; i++) {
		char *l = cached->lines[i];
		if (strlen(l) < 7 /* +++ b/ */)
			continue;

		const char *hdr = "+++ b/";
		int hdr_len = strlen(hdr);
		if (memcmp(l, hdr, hdr_len))
			continue;

		int nl = ret_nl_index(l);
		l[nl] = '\0';
		char *copied = strdup(l + hdr_len);
		if (!copied)
			die("strdup() failed\n");
		l[nl] = '\n';

		if (cached->nr_file_list == cached->file_list_size) {
			if (!cached->file_list_size) {
				assert(!cached->nr_file_list);
				cached->file_list_size = 8;
			} else
				cached->file_list_size <<= 1;

			cached->file_list = xrealloc(cached->file_list,
						cached->file_list_size * sizeof(char *));
		}

		cached->file_list[cached->nr_file_list++] = copied;
	}
}

static struct commit *size_order_head;

#define ALLOC_LIM (1 << 31)
static size_t total_alloced;

static void free_commits(size_t size)
{
	size_t freed = 0;

	for (struct commit *p = size_order_head; p; p = p->size_next) {
		struct commit_cached *pc = raw_get_cached(p);

		if (pc->state == CACHED_STATE_PURGED)
			continue;

		free(pc->text);
		pc->text = NULL;
		free(pc->lines);
		pc->lines = NULL;

		pc->state = CACHED_STATE_PURGED;

		freed += pc->text_size;
		if (size < freed)
			break;
	}

	total_alloced -= freed;
}

static void text_alloc(struct commit *c)
{
	struct commit_cached *cached = raw_get_cached(c);
	size_t size = cached->text_size;

	if (ALLOC_LIM < total_alloced + size)
		free_commits(size);

	total_alloced += size;
	cached->text = calloc(sizeof(char), size);
	if (!cached->text)
		die("memory allocation failed");

	if (c->size_order_initialized)
		goto end;

	if (!size_order_head) {
		size_order_head = c;
		goto end;
	}

	struct commit *size_prev = NULL;
	for (struct commit *p = size_order_head; p; p = p->size_next) {
		struct commit_cached *pc = raw_get_cached(p);

		if (!(pc->text_size < size)) {
			size_prev = p;
			continue;
		}

		if (!size_prev) {
			/* FIXME: this case should be handled before this loop */
			assert(p == size_order_head);

			c->size_next = size_order_head;
			size_order_head = c;

			goto end;
		}

		c->size_next = p;
		size_prev->size_next = c;

		goto end;
	}

	size_prev->size_next = c;
	c->size_order_initialized = true;

end:
	return;
}

static void read_commit_with_git_show(struct commit *c)
{
	char *read_from_fd(int fd, int *len)
	{
		static char *buf;
		static int buf_size;

		if (!buf) {
			buf_size = 1024;
			buf = xalloc(buf_size);
		}

		int rbytes = 0, ret;
		while ((ret = read(fd, buf + rbytes, buf_size - rbytes))) {
			if (ret == -1) {
				if (errno == EINTR)
					continue;

				die("read() failed\n");
			}

			rbytes += ret;

			if (rbytes == buf_size) {
				/* expand only */
				buf_size <<= 1;
				buf = xrealloc(buf, buf_size);
			}
		}

		*len = rbytes;
		return buf;
	}

	int pipefds[2];
	if (pipe(pipefds))
		die("pipe() failed\n");

	char *ret;
	pid_t pid = fork();
	switch (pid) {
	case 0:
		close(1);
		dup(pipefds[1]);
		close(pipefds[0]);
		if (execlp("git", "git", "show", c->commit_id, NULL))
			die("execlp() failed\n");

		break;

	case -1:
		die("fork() failed\n");
		break;

	default:
		close(pipefds[1]);
		ret = read_from_fd(pipefds[0], &c->cached.text_size);
		waitpid(pid, NULL, 0);
		close(pipefds[0]);
		break;
	}

	text_alloc(c);
	memcpy(c->cached.text, ret, c->cached.text_size);
}

static struct commit_cached *get_cached(struct commit *c)
{
	if (c->cached.state == CACHED_STATE_FILLED)
		return &c->cached;

	assert(!c->cached.text);
	read_commit_with_git_show(c);
	init_commit_lines(c);
	c->cached.state = CACHED_STATE_FILLED;

	return &c->cached;
}

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

static void update_terminal_default(void)
{
	move(0, 0);
	clear();

	struct commit_cached *cached = get_cached(current);

	int i;
	for (i = current->head_line;
	     i < current->head_line + row && i < cached->nr_lines; i++) {
		int j;
		char first_char;
		char *line;

		line = cached->lines[i];
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

	if (cached->nr_lines <= current->head_line + row)
		printw("100%%");
	else
		printw("% .0f%%",
			(float)(current->head_line + row)
			/ cached->nr_lines * 100.0);

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

struct help_str {
	char cmd;
	char *desc;
};

struct help_str help_str_array[] = {
#define cmd(cmd, func, desc) { cmd, desc },
#include "default_cmd.def"
#undef cmd
	{ '\0', NULL }
};

static void update_terminal_help(void)
{
	int i;

	move(0, 0);

	printw("keystorkes supported in default state\n\n");
	for (i = 0; help_str_array[i].cmd != '\0' ; i++) {
		printw("%c: %s\n", help_str_array[i].cmd,
			help_str_array[i].desc);
	}

	while (i++ < row)
		addch('\n');

	refresh();
}

static void update_terminal_show_changed_files(void)
{
	move(0, 0);

	printw("files changed in this commit:\n");

	struct commit_cached *cached = get_cached(current);
	int i;
	for (i = 0; i < cached->nr_file_list; i++) {
		printw(" %s\n", cached->file_list[i]);
	}

	while (i++ < row - 1)
		addch('\n');

	printw("type 'q' to quit this mode\n");
	refresh();
}


static void update_terminal(void)
{
	switch (state) {
	case STATE_DEFAULT:
	case STATE_INPUT_SEARCH_QUERY:
	case STATE_SEARCHING_QUERY:
	case STATE_INPUT_SEARCH_FILTER:
	case STATE_INPUT_SEARCH_FILTER2:
	case STATE_INPUT_SEARCH_DIRECTION:
	case STATE_LAUNCH_GIT_COMMAND:
	case STATE_READ_BRANCHNAME_FOR_CHECKOUT:
		update_terminal_default();
		break;

	case STATE_SHOW_CHANGED_FILES:
		update_terminal_show_changed_files();
		break;

	case STATE_HELP:
		update_terminal_help();
		break;

	default:
		die("unknown state: %d\n", state);
		break;
	};
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

static void read_commit(void)
{
	static int read_end;

	if (read_end)
		return;

	char commit_id[41];	/* with '\0' */

	int ret, rbytes = 0;

	while ((ret = read(stdin_fd, commit_id + rbytes, 41 - rbytes))) {
		if (ret == -1) {
			if (errno == EINTR)
				continue;

			die("read() failed\n");
		}

		rbytes += ret;
		if (rbytes == 41)
			break;
	}

	if (!ret)
		read_end = 1;
	else
		assert(commit_id[40] == '\n');

	commit_id[40] = '\0';

	struct commit *new_commit = xalloc(sizeof(*new_commit));

	if (tail) {
		assert(!tail->prev);

		tail->prev = new_commit;
		new_commit->next = tail;
		tail = new_commit;
	} else {
		/* very unlikely */
		assert(!head && !tail);

		current = head = tail = new_commit;
	}

	new_commit->commit_id = xalloc(41);
	memcpy(new_commit->commit_id, commit_id, 41);
	new_commit->cached.state = CACHED_STATE_PURGED;

	return;
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
	struct commit_cached *cached = get_cached(current);

	if (current->head_line + row < cached->nr_lines) {
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
	struct commit_cached *cached = get_cached(current);

	if (cached->nr_lines < row)
		return 0;

	current->head_line = cached->nr_lines - row;
	return 1;
}

static int forward_page(char cmd)
{
	struct commit_cached *cached = get_cached(current);

	if (cached->nr_lines < current->head_line + row)
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

	if (range_begin) {
		current = range_begin;
		return 1;
	}

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

	if (range_end)
		current = range_end;
	else
		current = head;

	return 1;
}

#define QUERY_SIZE 128
static char query[QUERY_SIZE + 1];
static int query_used;

static bool (*match_filter)(char *);

enum match_type {
	MATCH_TYPE_DEFAULT,
	MATCH_TYPE_MODIFIED,
	MATCH_TYPE_AT,
	MATCH_TYPE_COMMIT_MESSAGE,
	MATCH_TYPE_FILE,
};

static enum match_type current_match_type = MATCH_TYPE_DEFAULT;
static char *current_match_type_str(void)
{
	char *ret;

	switch (current_match_type) {
	case MATCH_TYPE_DEFAULT:
		ret = "default";
		break;
	case MATCH_TYPE_MODIFIED:
		ret = "modified";
		break;
	case MATCH_TYPE_AT:
		ret = "at";
		break;
	case MATCH_TYPE_COMMIT_MESSAGE:
		ret = "commit";
		break;
	case MATCH_TYPE_FILE:
		ret = "file";
		break;
	default:
		die("invalid match type: %d\n", current_match_type);
		break;
	};

	return ret;
}

static bool match_filter_modified(char *line)
{
	return line[0] == '+' || line[0] == '-';
}

static bool match_filter_at(char *line)
{
	return line[0] == '@';
}

static bool match_filter_file(char *line)
{
	/* lines begin with "+++" or "---" */
	if (strlen(line) < 3)
		return false;

	return !strncmp(line, "+++", 3) || !strncmp(line, "---", 3);
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

	struct commit_cached *cached = get_cached(c);

	if (prog) {
		if (direction) {
			if (cached->nr_lines <= i - 1)
				return 0;
		} else {
			if (!i)
				return 0;
		}

		i += direction ? 1 : -1;
	}

	do {
		line = cached->lines[i];
		nli = ret_nl_index(line);

		line[nli] = '\0';
		result = match_line(line);
		line[nli] = '\n';

		if (result) {
			c->head_line = i;
			return 1;
		}

		i += direction ? 1 : -1;
	} while (direction ? i < cached->nr_lines : 0 <= i);

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
		else {
			struct commit_cached *cached = get_cached(p);
			p->head_line = cached->nr_lines - 1;
		}

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
		bmprintf("%s %s search (type: %s): %s",			\
			current_direction ? "forward" : "backward",	\
			current_global ? "global" : "local",		\
			current_match_type_str(),			\
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
		current_match_type = MATCH_TYPE_DEFAULT;
		match_filter = match_filter_default;

	case STATE_INPUT_SEARCH_DIRECTION:
		query_used = 0;
		bzero(query, QUERY_SIZE);

		bmprintf("%s %s search (type: %s): ",
			current_direction ? "forward" : "backward",
			current_global ? "global" : "local",
			current_match_type_str());

		state = STATE_INPUT_SEARCH_QUERY;

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

static struct commit* get_prev_or_current(struct commit *c)
{
	if (c->prev)
		return c->prev;
	else {
		read_commit();
		if (c->prev)
			return c->prev;
	}

	/* hmm... */
	return c;
}

static int git_format_patch(void)
{
	struct commit *begin = range_begin;

	if (!begin)
		begin = current;

	if (!range_end)
		range_end = head;

	struct commit *prev_begin = get_prev_or_current(begin);

	char range[83];
	sprintf(range, "%s..%s", prev_begin->commit_id, range_end->commit_id);
	range[82] = '\0';

	endwin();
	printf("executing git... good luck!\n");

	/* TODO: options for format-patch */
	execlp("git", "git", "format-patch", "--cover-letter", range, NULL);
	die("execlp() failed\n");

	/* never reach */
	return 1;
}

static int git_rebase_i(void)
{
	struct commit *prev = get_prev_or_current(current);

	endwin();
	printf("executing git... good luck!\n");

	execlp("git", "git", "rebase", "-i", prev->commit_id, NULL);
	die("execlp() failed\n");

	/* never reach */
	return 1;
}

static int git_bisect(void)
{
	if (!range_begin || !range_end) {
		bmprintf("bisect begin and end are required before format-patch");
		state = STATE_DEFAULT;

		return 1;
	}

	struct commit *prev_range_begin = get_prev_or_current(range_begin);

	endwin();

	execlp("git", "git", "bisect", "start",
		range_end->commit_id, prev_range_begin->commit_id, NULL);
	die("execlp() failed\n");

	/* never reach */
	return 1;
}

static char checkout_branch_name[1024];
static int branch_name_idx;

static void git_checkout_b(void)
{
	endwin();
	printf("executing git... good luck\n");

	/* TODO: check existence of branch here, before execlp() */
	execlp("git", "git", "checkout", "-b",
		checkout_branch_name, current->commit_id, NULL);
	die("execlp() failed\n");
}

static int launch_git_command(char cmd)
{
	bmprintf("launch git command f (format-patch), r (rebase -i), c(checkout -b), b(bisect):");
	state = STATE_LAUNCH_GIT_COMMAND;
	return 1;
}

static int show_changed_files(char cmd)
{
	state = STATE_SHOW_CHANGED_FILES;
	return 1;
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

static int search_with_filter(char cmd)
{
	bmprintf("input search filter(m(modified), a(at line), f(+++, ---): ");
	if (cmd == ',')
		state = STATE_INPUT_SEARCH_FILTER;
	else
		state = STATE_INPUT_SEARCH_FILTER2;

	return 1;
}

static int help(char cmd)
{
	state = STATE_HELP;
	return 1;
}

struct key_cmd {
	char key;
	int (*op)(char);
};

static struct key_cmd valid_ops[] = {
#define cmd(cmd, func, desc) { cmd, func },
#include "default_cmd.def"
#undef cmd
	{ '\0', NULL },
};

static int search_filter_modified_line(char cmd)
{
	match_filter = match_filter_modified;
	current_match_type = MATCH_TYPE_MODIFIED;

	if (state == STATE_INPUT_SEARCH_FILTER2) {
		state = STATE_INPUT_SEARCH_DIRECTION;
		return search(1, 1);
	}

	state = STATE_INPUT_SEARCH_DIRECTION;

	bmprintf("type: %s, input search direction (/, ?, \\, !):",
		current_match_type_str());

	return 1;
}

static int search_filter_at_line(char cmd)
{
	match_filter = match_filter_at;
	current_match_type = MATCH_TYPE_AT;

	if (state == STATE_INPUT_SEARCH_FILTER2) {
		state = STATE_INPUT_SEARCH_DIRECTION;
		return search(1, 1);
	}

	state = STATE_INPUT_SEARCH_DIRECTION;

	bmprintf("type: %s, input search direction (/, ?, \\, !):",
		current_match_type_str());

	return 1;
}

static int search_filter_commit_message(char cmd)
{
	match_filter = match_filter_commit_message;
	current_match_type = MATCH_TYPE_COMMIT_MESSAGE;

	if (state == STATE_INPUT_SEARCH_FILTER2) {
		state = STATE_INPUT_SEARCH_DIRECTION;
		return search(1, 1);
	}

	state = STATE_INPUT_SEARCH_DIRECTION;

	bmprintf("type: %s(not implemented yet!), input search direction (/, ?, \\, !):",
		current_match_type_str());

	return 1;
}

static int search_filter_file_line(char cmd)
{
	match_filter = match_filter_file;
	current_match_type = MATCH_TYPE_FILE;

	if (state == STATE_INPUT_SEARCH_FILTER2) {
		state = STATE_INPUT_SEARCH_DIRECTION;
		return search(1, 1);
	}

	state = STATE_INPUT_SEARCH_DIRECTION;

	bmprintf("type: %s, input search direction (/, ?, \\, !):",
		current_match_type_str());

	return 1;
}

static int search_filter_cancel(char cmd)
{
	match_filter = match_filter_default;
	current_match_type = MATCH_TYPE_DEFAULT;
	state = STATE_DEFAULT;

	return 1;
}

static int search_filter_invalid(char cmd)
{
	bmprintf("invalid search type: %c\n", cmd);
	match_filter = match_filter_default;
	current_match_type = MATCH_TYPE_DEFAULT;
	state = STATE_DEFAULT;

	return 1;
}

static struct key_cmd search_filter_ops[] = {
	{ 'm', search_filter_modified_line },
	{ 'a', search_filter_at_line },
	{ 'l', search_filter_commit_message },
	{ 'f', search_filter_file_line },

	{ 0x1b, search_filter_cancel },

	{ '\0', NULL },
};

static int search_direction_cancel(char cmd)
{
	match_filter = match_filter_default;
	current_match_type = MATCH_TYPE_DEFAULT;
	state = STATE_DEFAULT;

	return 1;
}

static int search_direction_invalid(char cmd)
{
	bmprintf("invalid direction specifier: %c\n", cmd);
	state = STATE_DEFAULT;
	return 1;
}

static struct key_cmd search_direction_ops[] = {
	{ '/', search_global_forward },
	{ '?', search_global_backward },
	{ '\\', search_local_forward },
	{ '!', search_local_backward },

	{ 0x1b, search_direction_cancel },

	{ '\0', NULL },
};

static int (*ops_array[256])(char);
static int (*search_filter_ops_array[256])(char);
static int (*search_direction_ops_array[256])(char);

static void exit_handler(void)
{
	addch('\n');

#ifdef GIT_LESS_DEBUG
	/* unlink(DEBUG_FILE_NAME); */
#endif

	endwin();

	fprintf(stderr, dying_msg);
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

	read_commit();

	match_filter = match_filter_default;

	update_terminal();

	for (i = 0; i < 256; i++)
		ops_array[i] = nop;

	for (i = 0; valid_ops[i].key != '\0'; i++)
		ops_array[(int)valid_ops[i].key] = valid_ops[i].op;

	for (i = 0; i < 256; i++)
		search_filter_ops_array[i] = search_filter_invalid;

	for (i = 0; search_filter_ops[i].key != '\0'; i++)
		search_filter_ops_array[(int)search_filter_ops[i].key]
			= search_filter_ops[i].op;

	for (i = 0; i < 256; i++)
		search_direction_ops_array[i] = search_direction_invalid;

	for (i = 0; search_direction_ops[i].key != '\0'; i++)
		search_direction_ops_array[(int)search_direction_ops[i].key]
			= search_direction_ops[i].op;

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
		case STATE_INPUT_SEARCH_FILTER:
		case STATE_INPUT_SEARCH_FILTER2:
			ret = search_filter_ops_array[(int)cmd](cmd);
			break;

		case STATE_INPUT_SEARCH_QUERY:
			ret = input_query(cmd);
			break;

		case STATE_SEARCHING_QUERY:
		case STATE_DEFAULT:
			ret = ops_array[(int)cmd](cmd);
			break;

		case STATE_INPUT_SEARCH_DIRECTION:
			ret = search_direction_ops_array[(int)cmd](cmd);
			break;

		case STATE_LAUNCH_GIT_COMMAND:
			switch (cmd) {
			case 'f': /* format-patch */
				ret = git_format_patch();
				break;
			case 'r': /* interactive rebase */
				ret = git_rebase_i();
				break;
			case 'c': /* checkout -b */
				state = STATE_READ_BRANCHNAME_FOR_CHECKOUT;
				bmprintf("input branch name: ");
				break;
			case 'b':  /* bisect */
				ret = git_bisect();
				break;
			case 0x1b: /* escape */
				state = STATE_DEFAULT;
				break;
			}

			break;

		case STATE_READ_BRANCHNAME_FOR_CHECKOUT:
			if (cmd == (char)0x7f) {
				/* backspace */
				if (branch_name_idx)
					checkout_branch_name[--branch_name_idx] = '\0';
			} else if (cmd == (char)0x1b) {
				/* escape */
				memset(checkout_branch_name, 0, 1024);
				branch_name_idx = 0;
				state = STATE_DEFAULT;

				goto checkout_end;
			} else if (cmd == 0xd /* '\n' */) {
				git_checkout_b();
				goto checkout_end;
			} else {
				if (branch_name_idx < 1023)
					checkout_branch_name[branch_name_idx++] = cmd;
			}

			bmprintf("input branch name: %s", checkout_branch_name);
		checkout_end:

			break;

		case STATE_SHOW_CHANGED_FILES:
			if (cmd == 'q') {
				state = STATE_DEFAULT;
				ret = 1;
			}
			break;

		case STATE_HELP:
			if (cmd == 'q') {
				state = STATE_DEFAULT;
				ret = 1;
			} else
				ret = 0;

			break;
		default:
			die("invalid state: %d\n", state);
			break;
		}

		if (ret)
			update_terminal();

		prev_cmd = cmd;
	}

	return 0;
}
