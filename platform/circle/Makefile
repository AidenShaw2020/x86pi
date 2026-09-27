# Independent Circle target; never enters the RP2350 CMake build.
CIRCLEHOME ?= ../../build-rpi3-deps/circle
EXTRAINCLUDE += -I.
OBJS = main.o kernel.o
PCOBJ ?= pc
# GNU make's Circle configuration defines the built-in compiler variable PC=pc.
# Use the explicit value 1 so an ordinary invocation remains the infrastructure
# target and `PC=1` selects this full guest model.
ifeq ($(PC),1)
PCOBJ = pc
ifeq ($(PC_DIAG),1)
# Do not reuse normal-PC objects: Make does not encode command-line defines
# in its timestamps, so sharing pc/*.o could silently link diagnostic code
# into the normal image (or vice versa) after a mode switch.
PCOBJ = pc/diag
endif
OBJS = $(PCOBJ)/main.o $(PCOBJ)/kernel.o $(PCOBJ)/audio_circle.o $(PCOBJ)/hal.o $(PCOBJ)/compat.o \
	$(PCOBJ)/i386.o $(PCOBJ)/fpu.o $(PCOBJ)/libm.o $(PCOBJ)/pc.o $(PCOBJ)/i8042.o $(PCOBJ)/i8254.o $(PCOBJ)/i8257.o $(PCOBJ)/i8259.o \
	$(PCOBJ)/ne2000.o $(PCOBJ)/vga.o $(PCOBJ)/disk.o $(PCOBJ)/blockdev.o $(PCOBJ)/ide.o $(PCOBJ)/fdd.o $(PCOBJ)/pci.o $(PCOBJ)/misc.o \
	$(PCOBJ)/ini.o $(PCOBJ)/adlib.o $(PCOBJ)/sb16.o $(PCOBJ)/pcspk.o $(PCOBJ)/sn76489.o $(PCOBJ)/dss.o \
	$(PCOBJ)/gameport.o $(PCOBJ)/osd.o $(PCOBJ)/font8x16.o $(PCOBJ)/settingsui.o $(PCOBJ)/diskui.o $(PCOBJ)/config_save.o \
	$(PCOBJ)/opl3.o $(PCOBJ)/gmsynth.o $(PCOBJ)/ds3231.o $(PCOBJ)/frontpanel.o \
	$(PCOBJ)/bootlogo.o $(PCOBJ)/bootlogo_draw.o \
	$(PCOBJ)/jit.o
DEFINE += -DCIRCLE_BUILD=1 -DCIRCLE_DISK_WRITE=1 -DTINY386_PC=1 -DFRANK_NO_ADLIB=0 \
	-DNATIVE_JIT=0 -DBLOCK_JIT=0 -DTRACE_PORTS=0 -DSOUND_FREQUENCY=44100
# Periodic PERF/AUDIO/WAVE lines on the UART.  Off by default: they are
# measurement, not function, and even buffered they are work the guest
# does not need.  Build with STATS=1 when investigating.
# The translator.  Off by default: the interpreter is what is known to work,
# and nothing about it changes while this is unset.  Build with JIT=1.
# For a card that boots on its own: no log to the serial port and no polling
# it for keystrokes.  Both take the machine away from the guest - a log line
# is written a character at a time on a polled UART, and reading the receiver
# crosses a slow peripheral bus - and neither has a reader when nothing is
# attached.
ifeq ($(NOUART),1)
DEFINE += -DCIRCLE_PC_NO_UART=1
endif
ifeq ($(JIT),1)
DEFINE += -DTINY386_JIT=1
endif
ifeq ($(XLAT),1)
DEFINE += -DCIRCLE_PC_XLAT=1
endif
ifeq ($(JITPROF),1)
DEFINE += -DJIT_PROFILE=1
endif
ifeq ($(STATS),1)
DEFINE += -DCIRCLE_PC_STATS=1
endif
# Record the Sound Blaster's ports, DMA and interrupts; dumped with 0xF4.
ifeq ($(SBLOG),1)
DEFINE += -DSB_IO_LOG=1
endif
# Record the guest's OPL register writes for rendering off the board.
ifeq ($(OPLLOG),1)
DEFINE += -DADLIB_OPL_LOG=1
endif
# What each opcode costs.  A cycle counter read on the hot path, so its own
# switch: the numbers it gives are not comparable with a run without it.
# The processor's stall counters in place of the cache and branch ones.
ifeq ($(STALLS),1)
DEFINE += -DCIRCLE_PC_STATS=1 -DCIRCLE_PC_PMU_STALLS=1
endif
ifeq ($(OPCOST),1)
DEFINE += -DCIRCLE_PC_STATS=1 -DCIRCLE_PC_OPCOST=1
endif
GC_SECTIONS = 1
TARGET = kernel8-pc
ifeq ($(STACKWATCH),1)
# A watch on the stack slot Windows 95's disk detection finds overwritten;
# see CPU_STACK_WATCH in src/i386.c.  One compare on every store.
DEFINE += -DCPU_STACK_WATCH=1
endif
ifeq ($(XFERTRACE),1)
# Every change of code segment and of CR0.PE, kept in a ring for the
# exception dump; see CPU_XFER_TRACE in src/i386.c.  One compare per
# instruction.
DEFINE += -DCPU_XFER_TRACE=1
endif
ifeq ($(PC_DIAG),1)
# Diagnostic artifact only.  It is a separate target so a normal PC image
# cannot accidentally carry per-I/O instrumentation or UART rate logging.
DEFINE += -DCIRCLE_PC_DIAG=1
TARGET = kernel8-pc-diag
endif
endif
ifeq ($(SMOKE),1)
OBJS = smoke/main.o smoke/kernel.o smoke/cpu_smoke.o smoke/bios_probe.o smoke/ini.o smoke/runtime.o smoke/i386.o
DEFINE += -DTINY386_SMOKE=1
GC_SECTIONS = 1
TARGET = kernel8-smoke
endif
LIBS = $(CIRCLEHOME)/addon/SDCard/libsdcard.a \
       $(CIRCLEHOME)/addon/fatfs/libfatfs.a \
       $(CIRCLEHOME)/lib/fs/libfs.a \
       $(CIRCLEHOME)/lib/usb/libusb.a \
       $(CIRCLEHOME)/lib/input/libinput.a \
       $(CIRCLEHOME)/lib/sound/libsound.a \
       $(CIRCLEHOME)/lib/libcircle.a
include $(CIRCLEHOME)/Rules.mk
ifneq ($(AARCH),64)
$(error This initial target requires AARCH=64)
endif
ifneq ($(RASPPI),3)
$(error This initial target requires RASPPI=3)
endif
-include $(DEPS)

#
# Header dependencies.
#
# These rules used to name only the .c file, so nothing was rebuilt when a
# header changed.  That is not a tidiness problem: adding a field to the PC
# structure in pc.h moved every field after it, and the translation units that
# were not touched went on using the old offsets.  What that looked like was
# the settings menu reporting the Sound Blaster as enabled while the machine
# had it disabled - two pieces of code reading the same field at different
# addresses - and a day could be lost to a bug like that.
#
PCDEPS = $(PCOBJ)/*.d
-include $(PCDEPS)

#
# Flag changes.
#
# make compares timestamps, so switching STATS=1 on or off rebuilds nothing:
# the sources are older than the objects either way.  That produced a
# "diagnostics off" kernel that still had them compiled in, and a recording
# full of artefacts that were the measurement rather than the machine.
#
# The flags are written to a file that only changes when they do, and the
# objects depend on it.
#
#
# How the emulator itself is optimised, kept in one place so a different
# setting can be tried without editing a rule - and folded into the stamp
# below, so that trying one actually rebuilds something.
#
# I386OPT is for the interpreter alone.  Its dispatch is a computed goto,
# and the tails of those jumps are identical, so GCC merges them into a
# single indirect branch that the processor has no way to predict; the
# handlers want their own.
#
EMUOPT ?= -O3
I386OPT ?=
PCFLAGS_STAMP = $(PCOBJ)/.build-flags
$(shell mkdir -p $(PCOBJ);         echo '$(DEFINE) $(EMUOPT) $(I386OPT)' > $(PCFLAGS_STAMP).new;         cmp -s $(PCFLAGS_STAMP).new $(PCFLAGS_STAMP)           || mv $(PCFLAGS_STAMP).new $(PCFLAGS_STAMP);         rm -f $(PCFLAGS_STAMP).new)

# Every object, not only the C ones.  The rules below name the stamp
# themselves; the C++ files are built by Circle's generic rule, which does
# not, so turning JIT=1 on left pc/kernel.o compiled without it and the
# translator silently absent.  The same class of mistake this stamp exists
# for.
$(OBJS): $(PCFLAGS_STAMP)

smoke/i386.o: ../../src/i386.c
	$(CC) -MMD -MP $(CFLAGS) -fno-strict-aliasing -Ismoke/compat -I../../src -DNATIVE_JIT=0 -DBLOCK_JIT=0 -c $< -o $@

smoke/main.o: main.cpp kernel.h
	$(CPP) -MMD -MP $(CPPFLAGS) -c $< -o $@

smoke/kernel.o: kernel.cpp kernel.h
	$(CPP) -MMD -MP $(CPPFLAGS) -c $< -o $@

smoke/bios_probe.o: smoke/bios_probe.cpp smoke/bios_probe.h
	$(CPP) -MMD -MP $(CPPFLAGS) -c $< -o $@

smoke/ini.o: ../../src/ini.c ../../src/ini.h smoke/compat/stdio.h smoke/compat/ctype.h
	$(CC) -Ismoke/compat $(CFLAGS) -I../../src -c $< -o $@

$(PCOBJ)/jit.o: ../../src/jit/jit.c ../../src/jit/jit.h ../../src/jit/arm64_emit.h $(PCFLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) -MMD -MP $(CFLAGS) -include pc/compat/pico.h -Ipc -Ipc/compat -I../../src -c $< -o $@

# Its own variable rather than appending to EMUOPT: a variable set on make's
# command line beats a target-specific +=, so EMUOPT=-O3 on the line silently
# threw the interpreter's flags away and every measurement taken that way was
# the baseline wearing another name.
$(PCOBJ)/i386.o: I386EXTRA = $(I386OPT)
$(PCOBJ)/%.o: ../../src/%.c $(PCFLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) -MMD -MP $(CFLAGS) $(EMUOPT) $(I386EXTRA) -include pc/compat/pico.h -Ipc -Ipc/compat -I../../src -I../../build-rpi3-deps/circle/addon/fatfs -c $< -o $@

$(PCOBJ)/libm.o: pc/libm.c $(PCFLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) -MMD -MP $(CFLAGS) -c $< -o $@

# Nuked OPL3, LGPL-2.1: see src/nuked/LICENSE.
$(PCOBJ)/opl3.o: ../../src/nuked/opl3.c $(PCFLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) -MMD -MP $(CFLAGS) -Ipc/compat -c $< -o $@

# The boot logo and its unpacker.  Plain C with nothing behind it - no pico.h,
# no guest headers - so they get a rule of their own rather than the shared one.
$(PCOBJ)/bootlogo.o $(PCOBJ)/bootlogo_draw.o: $(PCOBJ)/%.o: pc/%.c $(PCFLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) -MMD -MP $(CFLAGS) -Ipc -c $< -o $@


$(PCOBJ)/main.o $(PCOBJ)/kernel.o $(PCOBJ)/audio_circle.o $(PCOBJ)/hal.o $(PCOBJ)/compat.o $(PCOBJ)/ds3231.o $(PCOBJ)/frontpanel.o:$(PCOBJ)/%.o: pc/%.cpp $(PCFLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CPP) -MMD -MP $(CPPFLAGS) -Ipc/compat -I../../src -I../../build-rpi3-deps/circle/addon/fatfs -c $< -o $@
