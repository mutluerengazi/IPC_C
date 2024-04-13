
CC	:= gcc
CFLAGS := -g -Wall

TARGETS :=  libmf.a  app1  app1-2 app2 app3 mfserver

# Make sure that 'all' is the first target
all: $(TARGETS)

MF_SRC :=  mf.c
MF_OBJS := $(MF_SRC:.c=.o)

libmf.a: $(MF_OBJS)
	ar rcs $@ $(MF_OBJS)

MF_LIB :=  -L.  -lmf -lrt -lpthread

mf.o: mf.c mf.h
	gcc -c $(CFLAGS) -o $@ mf.c

app1.o: app1.c  mf.c mf.h
	gcc -c $(CFLAGS)  -o $@ app1.c

app1: app1.o libmf.a mf.o
	gcc $(CFLAGS) -o $@ app1.o $(MF_LIB)

app1-2.o: app1-2.c  mf.c mf.h
	gcc -c $(CFLAGS)  -o $@ app1-2.c

app1-2: app1-2.o libmf.a mf.o
	gcc $(CFLAGS) -o $@ app1-2.o $(MF_LIB)

app2.o: app2.c  mf.c mf.h
	gcc -c $(CFLAGS)  -o $@ app2.c

app2: app2.o libmf.a mf.o
	gcc $(CFLAGS) -o $@ app2.o $(MF_LIB)
	
app3.o: app3.c  mf.c mf.h
	gcc -c $(CFLAGS)  -o $@ app3.c

app3: app3.o libmf.a mf.o
	gcc $(CFLAGS) -o $@ app3.o $(MF_LIB)

mfserver: mfserver.c  mf.c mf.h
	gcc -c $(CFLAGS)  -o $@ mfserver.c

mfserver: mfserver.o libmf.a mf.o
	gcc $(CFLAGS) -o $@ mfserver.o $(MF_LIB)

test: test.c
	gcc -g -Wall  -o  test test.c

clean:
	rm -rf core  *.o *.out *~ $(TARGETS)   app1 app1-2 app2 app3

	