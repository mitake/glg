#! /usr/bin/env python

import sys

file_size = 1024 * 1024 * 100   # 100MB
line_size = 80

line = 'a' * (line_size - 1) + '\n'
wbuf = line * (file_size / line_size)

f = open(sys.argv[1], 'w')
f.write(wbuf)
