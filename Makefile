ue: ue.c
	$(CC) -g -Wall -Os ue.c -c
	$(CC) -Os ue.o -o ue

test: ue
	cp ue.o ue-strip.o
	strip ue-strip.o
	@wc -c ue-strip.o
	@[ `wc -c ue-strip.o | cut -f1 -d ' '` -le 6144 ] && echo "OK!" || echo "TOO BIG!!!"

clean:
	rm -f ue *.o
