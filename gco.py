#! /usr/bin/env python
# gco: a wrapper of git checkout

import os, curses

heads = []
tags = []

def traverse_dir(result, dir_path, name_prefix):
    entries = os.listdir(dir_path)

    for e in entries:
        path = dir_path + '/' + e
        if os.path.isdir(path):
            traverse_dir(result, dir_path, name_prefix + '/' + e)
        else:
            result.append(name_prefix + e)

git_top_dir = os.getcwd() + '/.git'
heads_dir = git_top_dir + '/refs/heads'
tags_dir = git_top_dir + '/refs/tags'

traverse_dir(heads, heads_dir, '')
traverse_dir(tags, tags_dir, '')

all_refs = heads + tags
all_refs_len = len(all_refs)
selected_idx = 0

w = curses.initscr()
curses.start_color()
curses.nonl()
curses.cbreak()
curses.noecho()

def update_terminal():
    w.move(0, 0)
    w.clear()

    for i in range(0, all_refs_len):
        ref = all_refs[i]
        color = i == selected_idx

        if color:
            w.attron(curses.A_REVERSE)

        w.addstr(ref)
        w.addch('\n')

        if color:
            w.attroff(curses.A_REVERSE)

    w.refresh()

def cleaning():
    curses.endwin()

def checkout():
    ref = all_refs[selected_idx]
    cleaning()
    os.execlp('git', 'git', 'checkout', ref)

tty_f = open('/dev/tty', 'rb')

running = True
while running:
    update_terminal()
    key = tty_f.read(1)

    if key == 'j':
        if selected_idx + 1 == all_refs_len:
            continue
        
        selected_idx += 1
    elif key == 'k':
        if selected_idx == 0:
            continue
        
        selected_idx -= 1
    elif key == 'c':
        checkout()
        running = False
    elif key == 'q':
        running = False

cleaning()
