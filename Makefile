
ifndef DEBUG
# Default: compile for debug
DEBUG=1
endif

#PROFILE=1

# disable valgrind support
VALGRIND_FLAG=-DNVALGRIND

CC = gcc

BASICFLAGS= -pthread -std=c11 -fno-builtin-printf $(VALGRIND_FLAG)

DEBUGFLAGS=  -g3 
OPTFLAGS= -g3 -finline -march=native -O3 -DNDEBUG

ifeq ($(PROFILE),1)
PROFFLAGS= -g -pg 
PLFLAGS= -g -pg
else
PROFFLAGS= 
PLFLAGS=
endif

INCLUDE_PATH=-I.

CFLAGS= -Wall -D_GNU_SOURCE $(BASICFLAGS)

ifeq ($(DEBUG),1)
CFLAGS+=  $(DEBUGFLAGS) $(PROFFLAGS) $(INCLUDE_PATH)
else
CFLAGS+=  $(OPTFLAGS) $(PROFFLAGS) $(INCLUDE_PATH)
endif

LDFLAGS= $(PLFLAGS) $(BASICFLAGS)
LIBS=-lpthread -lrt -lm


C_PROG= test_util.c \
 	mtask.c tinyos_shell.c terminal.c \
 	validate_api.c \
 	$(EXAMPLE_PROG)

EXAMPLE_PROG= $(wildcard *_example*.c)

#
#  Add kernel source files here
#
C_SRC= bios.c $(wildcard kernel_*.c) tinyoslib.c symposium.c util.c unit_testing.c console.c
C_OBJ=$(C_SRC:.c=.o)

C_SOURCES= $(C_PROG) $(C_SRC)
C_OBJECTS=$(C_SOURCES:.c=.o)

FIFOS= con0 con1 con2 con3 kbd0 kbd1 kbd2 kbd3

.PHONY: all tests release clean distclean doc

all: mtask tinyos_shell terminal tests fifos examples

tests: test_util validate_api test_example 

examples: $(EXAMPLE_PROG:.c=) 


#
# Normal apps
#

mtask: mtask.o $(C_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

tinyos_shell: tinyos_shell.o $(C_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

terminal: terminal.o 
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)


#
# Tests
# 

test_util: test_util.o util.o unit_testing.o $(C_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

test_example: test_example.o util.o unit_testing.o $(C_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

validate_api: validate_api.o $(C_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

bios_example%: bios_example%.o bios.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)


# fifos

fifos: $(FIFOS)

$(FIFOS):
	mkfifo $@


doc: tinyos3.cfg $(wildcard *.h)
	doxygen tinyos3.cfg


distclean: realclean
	-touch .depend
	-rm *~

realclean:
	-rm $(C_PROG:.c=) $(C_OBJECTS) .depend
	-rm $(FIFOS)

depend: $(C_SOURCES)
	$(CC) $(CFLAGS) -MM $(C_SOURCES) > .depend

clean: realclean depend

include .depend

# Create release (courses handout) archive

release: clean-release-files tinyos3.tgz

clean-release-files:
	-rm tinyos3.tgz

tinyos3.tgz:
	$(MAKE) -C tinyos3 distclean
	tar czvhf tinyos3.tgz tinyos3

tinyos3-impl.tgz: distclean
	-rm tinyos3-impl.tgz
	tar czvhf tinyos3-impl.tgz *.c *.h Makefile

