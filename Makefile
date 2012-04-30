
OBJS = gitless.o
HDRS = die.h

CFLAGS = -O2 -Wall

CC = gcc

default: gitless gitless-invoker

gitless: $(OBJS)
	$(CC) -o gitless $(OBJS) -lncurses

%.o: %.c $(HDRS)
	$(CC) -c $(CFLAGS) $< -o $@

gitless-invoker: gitless-invoker.o $(HDRS)
	$(CC) -o gitless-invoker gitless-invoker.o

install: gitless gitless-invoker
	sudo cp gitless /usr/local/bin
	sudo cp gitless-invoker /usr/local/bin

cscope:
	find . -name "*.[ch]" > cscope.files
	cscope -b -q

clean:
	rm -f *.o
	rm -f cscope.*
