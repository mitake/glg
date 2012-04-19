
OBJS = gitless.o
HDRS =

CFLAGS = -O2 -Wall

CC = gcc

default: gitless

gitless: $(OBJS)
	$(CC) -o gitless $(OBJS) -lncurses

%.o: %.c $(HDRS)
	$(CC) -c $(CFLAGS) $< -o $@

install: gitless
	sudo cp gitless /usr/local/bin

cscope:
	find . -name "*.[ch]" > cscope.files
	cscope -b -q

clean:
	rm -f *.o
	rm -f cscope.*
