
OBJS = gitless.o
HDRS =

CFLAGS = -O2 -Wall -std=gnu99

CC = gcc

default: glg

glg: $(OBJS) default_cmd.def
	$(CC) -o glg $(OBJS) -lncurses -lX11

%.o: %.c $(HDRS)
	$(CC) -c $(CFLAGS) $< -o $@

install: glg
	sudo cp glg /usr/local/bin

cscope:
	find . -name "*.[ch]" > cscope.files
	cscope -b -q

clean:
	rm -f *.o
	rm -f cscope.*
