
default: gitless.c
	gcc -Wall -O2 -o gitless gitless.c

cscope:
	find . -name "*.[ch]" > cscope.files
	cscope -b -q

clean:
	rm gitless
	rm cscope.*
