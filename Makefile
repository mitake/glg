
OBJS = gitless.o vtparse.o vtparse_table.o
HDRS = vtparse.h

CFLAGS = -O2 -Wall

CC = gcc

default: $(OBJS)
	$(CC) -o gitless $(OBJS)

%.o: %.c $(HDRS)
	$(CC) -c $(CFLAGS) $< -o $@

cscope:
	find . -name "*.[ch]" > cscope.files
	cscope -b -q

clean:
	rm -f *.o
	rm cscope.*
