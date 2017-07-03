CFLAGS=`mysql_config --cflags` `pkg-config --cflags glib-2.0 gthread-2.0` `pcre-config --cflags` -O3 -g
LDFLAGS=`mysql_config --libs_r` `pkg-config --libs glib-2.0 gthread-2.0` `pcre-config --libs`

all: mydumper

mydumper: mydumper.o
	$(CC) -g -o mydumper mydumper.o $(LDFLAGS)

clean:
	rm -f mydumper dump *~ *BAK *.o

indent:
	gnuindent -ts4 -kr -l200 mydumper.c

