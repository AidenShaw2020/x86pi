CC ?= gcc
ROOT := ../../../..
SHIM := $(ROOT)/tests/hostbench/shim
CPPFLAGS := -Ishim -I$(SHIM) -I.. -I../compat -I$(ROOT)/src
CFLAGS := -O2 -g -std=gnu11 -fno-strict-aliasing -w \
          -DCIRCLE_BUILD=1 -DNATIVE_JIT=0 -DBLOCK_JIT=0 -include pico.h

BIN := diagnostic_regression
OBJS := diagnostic_regression.o i8254.o i8259.o misc.o i386.o

all: $(BIN)

diagnostic_regression.o: diagnostic_regression.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
i8254.o: $(ROOT)/src/i8254.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
i8259.o: $(ROOT)/src/i8259.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
misc.o: $(ROOT)/src/misc.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
i386.o: $(ROOT)/src/i386.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BIN): $(OBJS)
	$(CC) $(OBJS) -o $@

clean:
	rm -f $(BIN) $(OBJS)
.PHONY: all clean
