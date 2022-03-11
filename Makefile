ue: ue.c
	$(CC) -g -Wall -Os ue.c -c
	$(CC) -Os ue.o -o ue

test: ue
	cp ue.o ue-strip.o
	strip ue-strip.o
	echo "object size: `stat -c '%s' ue-strip.o`"
	@[ `stat -c '%s' ue-strip.o` -lt 10000 ] && echo "OK!"

clean:
	rm -f ue *.o
