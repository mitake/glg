
OBJS = glg.o
HDRS =

CFLAGS = -O2 -Wall -std=c++11

CPPC = clang++

default: glg

glg: $(OBJS) default_cmd.def
	$(CPPC) -o glg $(OBJS) -lncurses

%.o: %.cc $(HDRS)
	$(CPPC) -c $(CFLAGS) $< -o $@

install: glg
	sudo cp glg /usr/local/bin

cscope:
	find . -name "*.[ch]" > cscope.files
	cscope -b -q

clean:
	rm -f *.o
	rm -f cscope.*
