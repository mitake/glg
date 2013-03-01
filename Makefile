
OBJS = gitless.o
HDRS =

CFLAGS = -O2 -Wall -std=gnu99

CC = gcc

default: gitless

gitless: $(OBJS) default_cmd.def
	$(CC) -o gitless $(OBJS) -lncurses -lsqlite3 -lX11 # -lXmu

%.o: %.c $(HDRS)
	$(CC) -c $(CFLAGS) $< -o $@

install: gitless
	sudo cp gitless glg /usr/local/bin
	sudo cp gco.py /usr/local/bin/gco
	sudo chmod +x /usr/local/bin/gco

cscope:
	find . -name "*.[ch]" > cscope.files
	cscope -b -q

clean:
	rm -f *.o
	rm -f cscope.*
