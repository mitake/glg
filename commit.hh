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

