
CC = gcc
RM = rm -f

INCLUDES = 
#CFLAGS = -DDEBUG -g -O2 $(INCLUDES)
CFLAGS = -DNDEBUG -O2 $(INCLUDES)
LDFLAGS = 
LIBS = 

CSRCS = $(shell ls *.c)
OBJS = $(CSRCS:%.c=%.o)
#DEPENDS = $(OBJS:%.o=%.d)



.PHONY: all clean

all: test_sender uint2double

%.o: %.c
	gcc -o $@ -c $(CFLAGS) $<

test_sender: test_sender.o
	gcc -o $@ $< $(LDFLAGS) $(LIBS)

uint2double: uint2double.o
	gcc -o $@ $< $(LDFLAGS) $(LIBS)

clean:
	$(RM) *.o test_sender
	$(RM) *.o uint2double
