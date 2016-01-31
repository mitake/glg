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
#include <limits.h>
#include <sys/signalfd.h>
#include <poll.h>

#include <regex.h>
#include <ncurses.h>

#include <string>

using namespace std;

#define GIT_LESS_DEBUG
#define DEBUG_FILE_NAME "/tmp/git-less-debug"

extern int errno;

static int running = 1;

static char dying_msg[1024];

enum class commit_cached_state {
  PURGED,
  FILLED,
};

struct commit_cached {
  commit_cached_state state;

  char *text;
  unsigned int text_size;

  char **lines;
  int nr_lines, lines_size;
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

  char **file_list;
  int nr_file_list, file_list_size;

  char **commit_log;
  int commit_log_lines;
};

static struct commit *current;

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

#ifdef GIT_LESS_DEBUG
static int debug_fd;

#define dbgprintf(fmt, arg...)			\
  do {						\
    dprintf(debug_fd, "line %d: " fmt "\n",	\
	    __LINE__, ##arg);			\
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

  for (i = 0; s[i] != '\n'; i++)
    ;

  return i;
}

static int stdin_fd = 0, tty_fd;
static unsigned int row, col;

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

enum class long_run {
  DEFAULT,
  RUNNING,
  STOPPED,
};
static long_run state_long_run = long_run::DEFAULT;
/*
 * long_run_command(): called in the main loop when state_long_run == true
 * return 1 when the iteration ends. return 0 for continuing.
 */
static int (*long_run_command)(void);
/*
 * long_run_command_compl(): called after completion of long running command
 */
static void (*long_run_command_compl)(bool stopped);

enum class search_type {
  REGEX,
  FTS,
};
static search_type current_search_type;

#define BOTTOM_MESSAGE_INIT_SIZE 32
static char *bottom_message;
static unsigned int bottom_message_size = BOTTOM_MESSAGE_INIT_SIZE;

#define MATCH_ARRAY_INIT_SIZE 32
static regmatch_t *match_array;
static unsigned int match_array_size = MATCH_ARRAY_INIT_SIZE;

#define bmprintf(fmt, arg...)				\
  do {							\
    snprintf(bottom_message, bottom_message_size,	\
	     fmt, ##arg);				\
  } while (0)

#ifdef GIT_LESS_DEBUG

static FILE* debug_file;

#define debug_printf(fmt, arg...)		\
  do {						\
    fprintf(debug_file, fmt, ##arg);		\
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
    bottom_message = static_cast<char *>(xrealloc(bottom_message, bottom_message_size));
  }

  if (match_array_size < col) {
    match_array_size = col;
    match_array = static_cast<regmatch_t *>(xrealloc(match_array,
						     match_array_size * sizeof(regmatch_t)));
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

#define raw_get_cached(c) (&c->cached) /* simply get pointer */

int contain_visible_char(char *buf)
{
  int len = strlen(buf);

  for (int i = 0; i < len; i++)
    if (buf[i] != ' ') return i;

  return -1;
}

static void init_commit_lines(struct commit *c)
{
  struct commit_cached *cached = raw_get_cached(c);
  char *text = cached->text;
  int text_size = cached->text_size;

  cached->lines_size = LINES_INIT_SIZE;
  cached->lines = static_cast<char **>(xalloc(cached->lines_size * sizeof(char *)));

  char *line_head = text;
  cached->nr_lines = 0;
  for (int i = 0; i < text_size; i++) {
    if (text[i] != '\n')
      continue;

    cached->lines[cached->nr_lines++] = line_head;

    if (line_head[0] == 'c') {
      c->commit_id = static_cast<char *>(xalloc(41));
      memcpy(c->commit_id, line_head + 7 /* strlen("commit ") */, 40);
    }

    line_head = &text[i + 1];

    if (cached->lines_size == cached->nr_lines) {
      cached->lines_size <<= 1;
      cached->lines = static_cast<char **>(xrealloc(cached->lines,
						    cached->lines_size * sizeof(char *)));
    }
  }

  if (c->summary)
    return;

  for (int i = 0; i < cached->nr_lines; i++) {
    int j, len, nli;
    char *line;

    line = cached->lines[i];

    if ((j = contain_visible_char(line)) < 1)
      continue;

    nli = ret_nl_index(&line[j]);
    line[j + nli] = '\0';
    len = strlen(&line[j]);
    c->summary = static_cast<char *>(xalloc(len + 1));
    strcpy(c->summary, &line[j]);
    line[j + nli] = '\n';

    break;
  }

  const char *hdr = "+++ b/";
  int hdr_len = strlen(hdr);

  for (int i = 0; i < cached->nr_lines; i++) {
    char *l = cached->lines[i];
    if (ret_nl_index(l) < 7 /* +++ b/ */)
      continue;

    if (memcmp(l, hdr, hdr_len))
      continue;

    int nl = ret_nl_index(l);
    l[nl] = '\0';
    char *copied = strdup(l + hdr_len);
    if (!copied)
      die("strdup() failed\n");
    l[nl] = '\n';

    if (c->nr_file_list == c->file_list_size) {
      if (!c->file_list_size) {
	assert(!c->nr_file_list);
	c->file_list_size = 8;
      } else
	c->file_list_size <<= 1;

      c->file_list = static_cast<char **>(xrealloc(c->file_list,
						   c->file_list_size * sizeof(char *)));
    }

    c->file_list[c->nr_file_list++] = copied;
  }

  int i = 0;
  int commit_log_begin_idx = -1, commit_log_end_idx = -1;
  for (int state = 0; i < cached->nr_lines; i++) {
    char *l = cached->lines[i];

    switch (state) {
    case 0:
      if (!strlen(l))
	continue;

      if (l[0] == ' ') {
	state = 1; /* beginning of commit log */
	commit_log_begin_idx = i;
      }

      break;
    case 1:
      if (l[0] == '\n') {
	commit_log_end_idx = i;
	break;
      }

      break;
    default:
      die("unknown state of commit log parser: %d\n", state);
      break;
    }
  }

  if (commit_log_end_idx == -1)
    commit_log_end_idx = i - 1;

  assert(commit_log_begin_idx != -1 && commit_log_end_idx != -1);

  c->commit_log_lines = commit_log_end_idx - commit_log_begin_idx;
  c->commit_log = static_cast<char **>(xalloc(c->commit_log_lines * sizeof(char *)));
  for (int i = commit_log_begin_idx, j = 0; i < commit_log_end_idx; i++, j++) {
    char *copied, *l;

    l = cached->lines[i];
    int nl = ret_nl_index(l);
    l[nl] = '\0';
    copied= strdup(l);
    l[nl] = '\n';
    if (!copied)
      die("strdup() failed\n");

    c->commit_log[j] = copied;
  }
}

static struct commit *size_order_head;

#define ALLOC_LIM (1 << 30)
static size_t total_alloced;

static void free_commits(size_t size)
{
  size_t freed = 0;

  for (struct commit *p = size_order_head; p; p = p->size_next) {
    struct commit_cached *pc = raw_get_cached(p);

    if (pc->state == commit_cached_state::PURGED)
      continue;

    free(pc->text);
    pc->text = NULL;
    free(pc->lines);
    pc->lines = NULL;

    pc->state = commit_cached_state::PURGED;

    freed += pc->text_size;
    if (size < freed)
      break;
  }

  if (freed < size)
    die("memory allocation failed\n");

  total_alloced -= freed;
}

static void text_alloc(struct commit *c)
/* FIXME: clearly, I need a smart algorithm... */
{
  struct commit_cached *cached = raw_get_cached(c);
  size_t size = cached->text_size;

  if (ALLOC_LIM < total_alloced + size)
    free_commits(size);

  total_alloced += size;
  cached->text = static_cast<char *>(calloc(sizeof(char), size));
  if (!cached->text)
    die("memory allocation failed");

  if (c->size_order_initialized)
    return;

  if (!size_order_head) {
    size_order_head = c;
    c->size_order_initialized = true;
    return;
  }

  struct commit *size_prev = nullptr;
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

 end:
  c->size_order_initialized = true;
}

char *read_from_fd(int fd, unsigned int *len)
{
  static char *buf;
  static int buf_size;

  if (!buf) {
    buf_size = 1024;
    buf = static_cast<char *>(xalloc(buf_size));
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
      buf = static_cast<char *>(xrealloc(buf, buf_size));
    }
  }

  *len = rbytes;
  return buf;
}

static void read_commit_with_git_show(struct commit *c)
{
  int pipefds[2];
  if (pipe(pipefds))
    die("pipe() failed\n");

  char *ret = nullptr;
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
  if (c->cached.state == commit_cached_state::FILLED)
    return &c->cached;

  assert(!c->cached.text);
  read_commit_with_git_show(c);
  init_commit_lines(c);
  c->cached.state = commit_cached_state::FILLED;

  return &c->cached;
}

/* head: HEAD, root: root of the commit tree */
static struct commit *head, *root;
/* current: current displaying commit, tail: tail of the read commits */
static struct commit *tail;
static struct commit *range_begin, *range_end;

static regex_t *re_compiled;

static char **tokenized_query;
static int nr_tokenized_query, tokenized_query_size;

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

  int bm_len = strlen(bottom_message);

  int i;
  for (i = current->head_line;
       i < current->head_line + row - !!bm_len && i < cached->nr_lines;
       i++) {
    int j;
    char first_char;
    char *line;

    line = cached->lines[i];
    first_char = line[0];

    coloring(first_char, 1);

    if (state == STATE_SEARCHING_QUERY) {
      if (current_search_type == search_type::REGEX) {
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
	/* FIXME: disaster... */

	assert(current_search_type == search_type::FTS);

	int nli = ret_nl_index(line);
	for (int i = 0; i < nli; i++) {
	skip_token_print:

	  for (int j = 0; j < nr_tokenized_query; j++) {
	    char *q = tokenized_query[j];
	    int q_len = strlen(q);

	    if (nli - i < q_len)
	      continue;

	    if (strncmp(q, line + i, q_len))
	      continue;

	    attron(A_REVERSE);
	    for (int k = 0; k < q_len; k++)
	      addch(q[k]);
	    attroff(A_REVERSE);

	    i += q_len;
	    goto skip_token_print;
	  }

	  addch(line[i]);
	}
      }
    } else {
    normal_print:
      for (j = 0; j < col && line[j] != '\n'; j++)
	addch(line[j]);

    }

    addch('\n');
    coloring(first_char, 0);
  }

  while (i++ < current->head_line + row - !!bm_len)
    addch('\n');

  move(row - !!bm_len, 0);
  attron(A_REVERSE);

  char bm_buf[row + 1];	/* we are using C99 */
  char *p = bm_buf;

  if (cached->nr_lines <= current->head_line + row)
    snprintf(p, row, "100%%");
  else
    snprintf(p, row, "% .0f%%",
	     (float)(current->head_line + row)
	     / cached->nr_lines * 100.0);
  p += strlen(p);

  if (current->head_line + row < cached->nr_lines)
    snprintf(p, row - strlen(p), " (%d/%d)",
	     current->head_line + row, cached->nr_lines);
  else
    snprintf(p, row - strlen(p), " (%d/%d)",
	     cached->nr_lines, cached->nr_lines);
  p += strlen(p);

  snprintf(p, row - strlen(p), "   ");
  p += strlen(p);

  for (i = 0; i < 8; i++) {
    snprintf(p, row - strlen(p), "%c",
	     current->commit_id[i]);
    p += strlen(p);
  }
  snprintf(p, row - strlen(p), ": ");
  p += strlen(p);

  char summary[81];
  snprintf(summary, 80, "%s", current->summary);
  snprintf(p, row - strlen(p), "%s", summary);
  p += strlen(p);

  printw("%s", bm_buf);

  if (bm_len) {
    move(row, 0);
    printw("%s", bottom_message);
  }

  attroff(A_REVERSE);

  refresh();
}

struct help_str {
  char cmd;
  char *desc;
};

struct help_str help_str_array[] = {
#define cmd(cmd, func, desc) { cmd, static_cast<char *>(const_cast<char *>(desc)) },
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

  int i;
  for (i = 0; i < current->nr_file_list; i++) {
    printw(" %s\n", current->file_list[i]);
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
    if (state_long_run == long_run::RUNNING)
      /* FIXME: use signalfd */
      state_long_run = long_run::STOPPED;
    break;

  default:
    die("unknown signal: %d", signum);
    break;
  }
}

static int init_signalfd(void)
{
  sigset_t mask;
  int sigfd;

  sigemptyset(&mask);
  sigaddset(&mask, SIGWINCH);
  sigaddset(&mask, SIGINT);
  sigprocmask(SIG_BLOCK, &mask, NULL);

  sigfd = signalfd(-1, &mask, SFD_NONBLOCK);
  if (sigfd < 0) {
    fprintf(stderr, "signalfd() failed: %s\n",
	    strerror(errno));
    exit(1);
  }

  return sigfd;
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

  struct commit *new_commit = static_cast<struct commit *>(xalloc(sizeof(*new_commit)));

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

  new_commit->commit_id = static_cast<char *>(xalloc(41));
  memcpy(new_commit->commit_id, commit_id, 41);
  new_commit->cached.state = commit_cached_state::PURGED;

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
  if (cached->nr_lines < current->head_line + row)
    current->head_line = cached->nr_lines - row;

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

static struct commit *orig_before_visit_root;

static int long_run_command_visit_root(void)
{
  if (!current->prev)
    read_commit();

  if (!current->prev) {
    long_run_command = NULL;
    return 1;
  }

  current = current->prev;
  return 0;
}

static void long_run_command_compl_visit_root(bool stopped)
{
  if (!stopped)
    goto end;

  current = orig_before_visit_root;
  bmprintf("stop visiting root commit");

 end:
  orig_before_visit_root = NULL;
  memset(bottom_message, 0, bottom_message_size);

  long_run_command = NULL;
  long_run_command_compl = NULL;
  state_long_run = long_run::DEFAULT;
}

static int show_root(char cmd)
{
  if (range_begin) {
    current = range_begin;
    return 1;
  }

  if (root) {
    current = root;
    current->head_line = 0;

    return 1;
  }

  orig_before_visit_root = current;

  assert(!long_run_command);
  assert(!long_run_command_compl);
  long_run_command = long_run_command_visit_root;
  long_run_command_compl = long_run_command_compl_visit_root;

  assert(state_long_run == long_run::DEFAULT);
  state_long_run = long_run::RUNNING;

  bmprintf("visiting root commit...");
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

  current->head_line = 0;

  return 1;
}

#define QUERY_SIZE 128
static char query[QUERY_SIZE + 1];
static int query_used;

static bool (*match_filter)(char *);

enum class match_type {
  DEFAULT,
  MODIFIED,
  AT,
  COMMIT_MESSAGE,
  FILE,
};

static match_type current_match_type = match_type::DEFAULT;
static char *current_match_type_str(void)
{
  string ret;

  switch (current_match_type) {
  case match_type::DEFAULT:
    ret = "default";
    break;
  case match_type::MODIFIED:
    ret = "modified";
    break;
  case match_type::AT:
    ret = "at";
    break;
  case match_type::COMMIT_MESSAGE:
    ret = "commit";
    break;
  case match_type::FILE:
    ret = "file";
    break;
  default:
    die("invalid match type: %d\n", current_match_type);
    break;
  };

  return const_cast<char *>(ret.c_str());
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

static int match_commit_regex(struct commit *c, int direction, int prog)
{
  int i = c->head_line;
  int nli, result;
  char *line;

  struct commit_cached *cached = get_cached(c);

  if (prog) {
    /* exclude current line */

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

static int match_commit(struct commit *c, int direction, int prog)
{
  return match_commit_regex(c, direction, prog);
}

static int current_direction, current_global;

#define update_query_bm()	do {				\
    bmprintf("%s %s search (filter: %s, type: %s): %s",		\
	     current_direction ? "forward" : "backward",	\
	     current_global ? "global" : "local",		\
	     current_match_type_str(),				\
	     current_search_type == search_type::REGEX ?		\
	     "regex" : "FTS",					\
	     query);						\
								\
  } while (0)

static bool search_found;
static struct commit *orig_before_do_search;
static void long_run_command_compl_do_search(bool stopped)
{
  if (search_found)
    goto end;

  if (!stopped) {
    if (!search_found)
      bmprintf("not found: %s", query);

    goto restore;
  }

  bmprintf("search stopped");

 restore:
  current = orig_before_do_search;

 end:
  search_found = false;
  orig_before_do_search = NULL;
}

static int long_run_command_do_search(void)
{
  int result = 0;

  result = match_commit(current, current_direction, 0);
  if (result) {
    if (current_search_type == search_type::FTS)
      current->head_line = 0;

    search_found = true;

    return 1;
  }

  if (current_direction) {
    if (current == range_begin)
      goto not_found;

    if (!current->prev)
      read_commit();

    if (current->prev)
      current = current->prev;
    else
      goto not_found;
  } else {
    if (current == range_end)
      goto not_found;

    if (current->next)
      current = current->next;
    else
      goto not_found;

    current->head_line = get_cached(current)->nr_lines - 1;
  }

  return 0;

 not_found:
  bmprintf("not found: %s", query);
  search_found = false;

  return 1;
}

static int do_search(int direction, int global, int prog)
{
  int result;

  result = match_commit(current, direction, prog);
  if (result)
    return 1;
  if (!global)
    return 0;

  orig_before_do_search = current;

  if (direction) {
    if (current == range_begin)
      return 0;

    if (!current->prev)
      read_commit();

    if (current->prev)
      current = current->prev;
    else
      return 0;
  } else {
    if (current == range_end)
      return 0;

    if (current->next)
      current = current->next;
    else
      return 0;

    current->head_line = get_cached(current)->nr_lines - 1;
  }

  current_direction = direction;
  current_global = global;

  assert(!long_run_command);
  assert(!long_run_command_compl);
  long_run_command = long_run_command_do_search;
  long_run_command_compl = long_run_command_compl_do_search;

  assert(state_long_run == long_run::DEFAULT);
  state_long_run = long_run::RUNNING;
  search_found = false;

  return -1;
}

static struct {
  struct commit *commit;
  int head_line;
} orig_place;

static void tokenize_query(void)
{
  static char *duped_query;

  if (duped_query)
    free(duped_query);

  duped_query= strdup(query);
  if (!duped_query)
    die("strdup() failed\n");

  if (!tokenized_query) {
    nr_tokenized_query = 0;
    tokenized_query_size = 1;
    tokenized_query = static_cast<char **>(xalloc(tokenized_query_size * sizeof(char *)));
  }

  char *saveptr;
  tokenized_query[0] = strtok_r(duped_query, " ", &saveptr);
  nr_tokenized_query = 1;

  char *next_token;
  while ((next_token = strtok_r(NULL, " ", &saveptr))) {
    if (nr_tokenized_query == tokenized_query_size) {
      tokenized_query_size *= 2;
      tokenized_query =
	static_cast<char **>(xrealloc(tokenized_query, tokenized_query_size * sizeof(char *)));
    }

    tokenized_query[nr_tokenized_query++] = next_token;
  }
}

static int _search(int key, int direction, int global)
{
  current_direction = direction;
  current_global = global;

  switch (state) {
  case STATE_DEFAULT:
  case STATE_SEARCHING_QUERY:
    current_match_type = match_type::DEFAULT;
    match_filter = match_filter_default;

  case STATE_INPUT_SEARCH_DIRECTION:
    query_used = 0;
    bzero(query, QUERY_SIZE);

    bmprintf("%s %s search (filter: %s, type: %s): ",
	     current_direction ? "forward" : "backward",
	     current_global ? "global" : "local",
	     current_match_type_str(),
	     current_search_type == search_type::REGEX ? "regex" : "FTS");

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

      if (current_search_type == search_type::FTS)
	tokenize_query();

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
    if (current_search_type == search_type::REGEX) {
      if (re_compiled)
	regfree(re_compiled);
      else
	re_compiled = static_cast<regex_t *>(xalloc(sizeof(regex_t)));
      regcomp(re_compiled, query, REG_ICASE);
    } else {
      assert(current_search_type == search_type::FTS);
    }

    int result = do_search(direction, global, 0);
    switch (result) {
    case 0:
      bmprintf("not found: %s", query);
      break;
    case 1:
      update_query_bm();
      break;
    case -1:	/* do nothing, continue */
      assert(state_long_run == long_run::RUNNING);
      return 0;
    default:
      die("invalid return value from do_search(): %d\n", result);
      break;
    }
  }

  return 1;
}

static int search(int direction, int global)
{
  return _search(-1, direction, global);
}

static int search_global_forward(char cmd)
{
  current_search_type = search_type::REGEX;
  return search(1, 1);
}

static int search_global_backward(char cmd)
{
  current_search_type = search_type::REGEX;
  return search(0, 1);
}

static int search_local_forward(char cmd)
{
  current_search_type = search_type::REGEX;
  return search(1, 0);
}

static int search_local_backward(char cmd)
{
  current_search_type = search_type::REGEX;
  return search(0, 0);
}

static int search_progress(char cmd)
{
  if (state != STATE_SEARCHING_QUERY)
    return 0;

  assert(cmd == 'n' || cmd == 'p');

  int res = do_search(cmd == 'n' ? 1 : 0, current_global, 1);
  switch (res) {
  case 0:
    bmprintf("not found: %s", query);
  case 1:
    return 1;
  case -1:
    update_query_bm();
    return 0;
  default:
    die("invalid return value of do_search(): %d", res);
    return 0;	/* suppress compiler warning */
  }
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
    if (current_search_type == search_type::REGEX
	&& re_compiled) {
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

static int git_format_patch(bool force)
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

  int ret;
  char key = ' ';
  bool retry = false;
 retry_cover_letter:;
  bool need_cover_letter = false;
  move(row, 0);
  for (int i = 0; i < col; i++) printw(" ");
  move(row, 0);
  printw("%s%c need cover letter? (y/N): ", retry ? "invalid char: " : "", key);
  refresh();

  ret = read(tty_fd, &key, 1);
  if (ret != 1)
    die("read() failed");
  switch (key) {
  case 'y':
  case 'Y':
    need_cover_letter = true;
    break;
  case 'n':
  case 'N':
  case 0xd:		/* enter */
    need_cover_letter = false;
    break;
  case 0x1b:		/* escape */
    /* cancel of format-patch */
    return 1;
  default:
    retry = true;
    goto retry_cover_letter;
  }

  char prefix[32];
  int prefix_i = 0;
  memset(prefix, 0, 32);
 read_prefix:
  move(row, 0);
  for (int i = 0; i < col; i++) printw(" ");
  move(row, 0);
  printw("prefix of the patchset%s: %s", prefix_i == 31 ? "max" : "", prefix);
  refresh();

  ret = read(tty_fd, &key, 1);
  if (ret != 1)
    die("read() failed");

  if (key == (char)0x7f) {
    /* backspace */
    if (prefix_i)
      prefix[--prefix_i] = '\0';
    goto read_prefix;
  } else if (key == (char)0xd) {
    /* enter */
    goto end_read_prefix;
  } else if (key == 0x1b) {
    /* escape, cancel of format-patch */
    return 1;
  }


  if (prefix_i == 31) {
    /* prefix is too long, simply ignore the inputted char */
    goto read_prefix;
  }

  prefix[prefix_i++] = key;

  goto read_prefix;

 end_read_prefix:
  endwin();
  printf("executing git... good luck!\n");
  fflush(stdout);

  if (!force) {
    /* FIXME: how should I treat a case of IP unreachable? */
    pid_t pid = fork();
    switch (pid) {
    case -1:
      die("fork() failed\n");
      break;
    case 0:;
      int pid2;
      pid2 = fork();
      switch (pid2) {
      case 0:
	execlp("git", "git", "remote", "update", NULL);
	break;
      case -1:
	die("fork() failed\n");
	break;
      default:
	waitpid(pid2, NULL, 0);
	break;
      }
      /* FIXME: "origin/master" is always suitable? */
      execlp("git", "git", "rebase", "origin/master", NULL);
      break;
    default:;
      int status;
      waitpid(pid, &status, 0);
      if (WEXITSTATUS(status)) {
	/* if rebase found something, it returns 1 */
	printf("rebase found something, you should check before posting patch!\n");
	exit(0);
      }

      break;
    }
  }

  static char *execvp_args[6];
  int execvp_args_i = 0;
  execvp_args[execvp_args_i++] = const_cast<char *>("git");
  execvp_args[execvp_args_i++] = const_cast<char *>("format-patch");
  if (need_cover_letter)
    execvp_args[execvp_args_i++] = const_cast<char *>("--cover-letter");
  if (strlen(prefix)) {
    static char subject_prefix[64];
    snprintf(subject_prefix, 64, "--subject-prefix=%s", prefix);
    execvp_args[execvp_args_i++] = subject_prefix;
  }
  execvp_args[execvp_args_i++] = const_cast<char *>("-C");
  execvp_args[execvp_args_i++] = range;
  execvp_args[execvp_args_i++] = NULL;

  execvp("git", execvp_args);
  die("execvp() failed\n");

  /* never reach */
  return 1;
}

static int git_rebase_i(void)
{
  struct commit *prev = get_prev_or_current(current);

  endwin();
  printf("executing git... good luck!\n");
  fflush(stdout);

  close(stdin_fd);
  stdin_fd = open("/dev/tty", O_RDONLY);
  if (stdin_fd != 0)
    die("failed to open /dev/tty\n");

  close(1);
  int stdout_fd = open("/dev/tty", O_WRONLY);
  if (stdout_fd != 1)
    die("failed to open /dev/tty]n\n");
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

static int git_revert(void)
{
  endwin();
  execlp("git", "git", "revert", current->commit_id, NULL);
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
  fflush(stdout);

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

enum class range_state {
  INIT,
  BEGIN_SPECIFIED,
  END_SPECIFIED,
  SPECIFIED
};
static range_state range_state = range_state::INIT;

static int specify_range(char cmd)
{
  int begin_set, end_set;

  begin_set = end_set = 0;

  switch (range_state) {
  case range_state::INIT:
    if (cmd == '[') {
      range_begin = current;
      range_state = range_state::BEGIN_SPECIFIED;

      begin_set = 1;
    } else {
      assert(cmd == ']');

      range_end = current;
      range_state = range_state::BEGIN_SPECIFIED;

      end_set = 1;
    }

    break;
  case range_state::BEGIN_SPECIFIED:
    if (cmd == '[') {
      range_begin = current;
      range_state = range_state::BEGIN_SPECIFIED;

      begin_set = 1;
    } else {
      assert(cmd == ']');

      range_end = current;
      range_state = range_state::SPECIFIED;

      end_set = 1;
    }

    break;
  case range_state::END_SPECIFIED:
    if (cmd == '[') {
      range_begin = current;
      range_state = range_state::SPECIFIED;

      begin_set = 1;
    } else {
      assert(cmd == ']');

      range_end = current;
      range_state = range_state::END_SPECIFIED;

      end_set = 1;
    }

    break;
  case range_state::SPECIFIED:
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

  if (range_state == range_state::SPECIFIED) {
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
  range_state = range_state::INIT;

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

static int stop_search(char cmd)
{
  if (state == STATE_SEARCHING_QUERY) {
    state = STATE_DEFAULT;
    memset(bottom_message, 0, bottom_message_size);
  }

  return 1;
}

static int clipboard_pid;

char *copy_with_editor(struct commit *c, int *buf_len)
{
  struct commit_cached *cached = get_cached(c);
  char tmp_path[PATH_MAX];

  strcpy(tmp_path, "/tmp/gitless-yank-XXXXXX");
  mkstemp(tmp_path);

  int tmp_fd = open(tmp_path, O_WRONLY);
  if (tmp_fd < 0)
    die("open() failed\n");

  int wbytes = 0;
  do {
    int wret = write(tmp_fd, cached->text + wbytes, cached->text_size - wbytes);
    if (wret < 0)
      die("write() failed\n");
    wbytes += wret;
  } while (wbytes < cached->text_size);
  close(tmp_fd);

  char *env_editor = getenv("EDITOR");
  int editor_pid = fork();
  switch (editor_pid) {
  case 0:
    execlp(env_editor, env_editor, tmp_path, NULL);
    break;
  case -1:
    die("fork() failed\n");
    break;
  default:
    waitpid(editor_pid, NULL, 0);
    break;
  }

  tmp_fd = open(tmp_path, O_RDONLY);
  struct stat tmp_stat;

  memset(&tmp_stat, 0, sizeof(tmp_stat));
  int fret = fstat(tmp_fd, &tmp_stat);
  if (fret < 0)
    die("fstat() failed\n");

  char *ret = static_cast<char *>(xalloc(tmp_stat.st_size));
  int rbytes = 0;
  do {
    int rret = read(tmp_fd, ret + rbytes, tmp_stat.st_size - rbytes);
    if (rret < 0)
      die("read() failed\n");
    rbytes += rret;
  } while (rbytes < tmp_stat.st_size);
  close(tmp_fd);
  unlink(tmp_path);

  *buf_len = tmp_stat.st_size;
  return ret;
}

void yank_with_xclip(char *buf, int buf_len, const char *board) {
  int pipefds[2];

  if (pipe(pipefds))
    die("pipe() failed\n");

  int xclip_pid = fork();
  switch (xclip_pid) {
  case 0:
    break;
  case -1:
    die("fork() failed\n");
    break;
  default:
    close(pipefds[0]);

    int wbytes = 0;
    do {
      int wret = write(pipefds[1], buf + wbytes, buf_len - wbytes);
      if (wret < 0)
	die("write() failed\n");
      wbytes += wret;
    } while (wbytes < buf_len);
    close(pipefds[1]);

    return;
  }

  close(0);
  dup(pipefds[0]);
  close(pipefds[1]);
  if (execlp("xclip", "xclip", "-i", "-selection", board, NULL))
    die("execlp() failed\n");
}

static int yank(char cmd)
{
  if (clipboard_pid)
    kill(clipboard_pid, SIGKILL);

  char yank_target;
  move(row, 0);
  printw("yank what? c (commit ID), e (entire with editor) :");
  refresh();

  int ret = read(tty_fd, &yank_target, 1);
  if (ret != 1)
    die("read() failed\n");
  if (yank_target != 'c' || yank_target != 'e') {
  }

  char *copy_buf;
  int copy_buf_len;

  switch (yank_target) {
  case 'c':
    copy_buf = strdup(current->commit_id);
    copy_buf_len = 41;
    break;
  case 'e':
    copy_buf = copy_with_editor(current, &copy_buf_len);
    break;
  default:
    bmprintf("unknown yank target: %c\n", yank_target);
    return 1;
  }

  yank_with_xclip(copy_buf, copy_buf_len, "CLIPBOARD");
  yank_with_xclip(copy_buf, copy_buf_len, "PRIMARY");

  free(copy_buf);

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
  current_match_type = match_type::MODIFIED;

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
  current_match_type = match_type::AT;

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
  current_match_type = match_type::COMMIT_MESSAGE;

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
  current_match_type = match_type::FILE;

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
  current_match_type = match_type::DEFAULT;
  state = STATE_DEFAULT;

  return 1;
}

static int search_filter_invalid(char cmd)
{
  bmprintf("invalid search type: %c\n", cmd);
  match_filter = match_filter_default;
  current_match_type = match_type::DEFAULT;
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
  current_match_type = match_type::DEFAULT;
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

  if (clipboard_pid)
    kill(clipboard_pid, SIGKILL);

  endwin();

  fprintf(stderr, dying_msg);
}

static void launch_git_log(void)
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
    close(0);
    dup(pipefds[0]); /* connect git log to stdin */
    break;
  }
}

int main(void)
{
  int i, sigfd;
  char cmd;
  struct pollfd pfds[2];

#ifdef GIT_LESS_DEBUG

  unlink(DEBUG_FILE_NAME);
  debug_fd = open(DEBUG_FILE_NAME, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
  if (debug_fd < 0)
    die("failed to open() file: %s for debugging", DEBUG_FILE_NAME);

  debug_file = fdopen(debug_fd, "w");
  if (!debug_file)
    die("failed fdopen() for debug_file");

#endif

  launch_git_log();

  bottom_message = static_cast<char *>(xalloc(bottom_message_size));
  match_array = static_cast<regmatch_t *>(xalloc(match_array_size * sizeof(regmatch_t)));

  atexit(exit_handler);

  sigfd = init_signalfd();
  init_tty();

  memset(pfds, 0, sizeof(pfds));

  pfds[0].fd = sigfd;
  pfds[0].events = POLLIN;

  pfds[1].fd = tty_fd;
  pfds[1].events = POLLIN;

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
    int ret = 0, pret;

    pret = poll(pfds, 2, state_long_run == long_run::RUNNING ? 0 : -1);
    if (pret < 0)
      die("poll() failed");

    if (pfds[0].revents & POLLIN) {
      struct signalfd_siginfo siginfo;
      int rbytes;

      rbytes = read(pfds[0].fd, &siginfo, sizeof(siginfo));
      if (rbytes != sizeof(siginfo))
	die("reading siginfo failed\n");

      signal_handler(siginfo.ssi_signo);
    }

    if (state_long_run != long_run::DEFAULT) {
      assert(long_run_command);
      assert(long_run_command_compl);

      switch (state_long_run) {
      case long_run::RUNNING:
	if (long_run_command()) {
	  long_run_command_compl(false);
	  goto long_run_end;
	}

	continue;
      case long_run::STOPPED:
	long_run_command_compl(true);

      long_run_end:
	state_long_run = long_run::DEFAULT;
	long_run_command = NULL;
	long_run_command_compl = NULL;

	ret = 1;

	break;
      default:
	die("unknown state_long_run: %d", state_long_run);
	break;
      }
    }

    if (pfds[1].revents & POLLIN) {
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
	case 'F':
	  ret = git_format_patch(cmd == 'F');
	  /* return of this function means format-patch is canceled */
	  state = STATE_DEFAULT;
	  memset(bottom_message, 0, bottom_message_size);

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
	case 'R': /* revert */
	  ret = git_revert();
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

	  memset(bottom_message, 0, bottom_message_size);

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
    }

    if (ret)
      update_terminal();
  }

  return 0;
}
