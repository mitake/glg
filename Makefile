
default: gitless

gitless: gitless.c tty.c misc.h tty.h
	gcc -Wall -O2 -o gitless gitless.c tty.c

install: gitless
	sudo cp gitless /usr/local/bin/

cscope:
	find . -name "*.[ch]" > cscope.files
	cscope -b -q

clean:
	rm gitless
	rm cscope.*
