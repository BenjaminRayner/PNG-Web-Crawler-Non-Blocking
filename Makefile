# Makefile, ECE252  
# Yiqing Huang

CC = gcc 
CFLAGS_LIBS = $(shell pkg-config --cflags libxml-2.0 libcurl)
CFLAGS = -Wall $(CFLAGS_LIBS) -std=gnu99 -g
LD = gcc
LDFLAGS = -std=gnu99 -g 
LDLIBS = $(shell pkg-config --libs libxml-2.0 libcurl)

SRCS   = findpng3.c
OBJS1  = findpng3.o
TARGETS = findpng3

all: ${TARGETS}

findpng3: $(OBJS1)
	$(LD) -o $@ $^ $(LDLIBS) $(LDFLAGS) 

%.o: %.c 
	$(CC) $(CFLAGS) -c $< 

%.d: %.c
	gcc -MM -MF $@ $<

-include $(SRCS:.c=.d)

.PHONY: clean
clean:
	rm -f *~ *.d *.o $(TARGETS) 
