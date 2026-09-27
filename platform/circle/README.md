# X86Pi on Circle - development notes

The Raspberry Pi target: a bare-metal AArch64 kernel for the Pi 3 built on
[Circle](https://github.com/rsta2/circle) revision
`6177984e30fac5e65582d171d43f1563368a94ac`. The emulator itself is in `../../src`;
this directory is the board side.

| Path | What it is |
|---|---|
| `pc/kernel.cpp` | Start-up, the main loop on core 0, the other three cores (`CSynthCores`), USB input, the serial link, power handling |
| `pc/audio_circle.cpp` | The mixer, feeding HDMI and the 3.5 mm jack |
| `pc/hal.cpp`, `pc/compat.cpp`, `pc/compat/` | What the shared sources expect of the platform |
| `pc/ds3231.cpp` | DS3231 real-time clock on I2C1 |
| `pc/frontpanel.cpp` | Power and reset buttons, power and HDD LEDs |
| `pc/bootlogo*.c`, `tools/mklogo.py` | The logo shown while the machine starts |
| `patches/` | Circle source files that `build.sh` copies over the checkout (USB floppy, mass storage and CD, hub) |
| `main.cpp`, `kernel.cpp`, `smoke/` | A small bring-up kernel: `make` without `PC=1` |
| `debug/` | Tools for working on the board remotely |

## Building

`fetch-circle.sh` clones Circle at the pinned revision and installs
`Config.mk.example` as its `Config.mk`; `build.sh` refuses any other revision or
configuration, applies X86Pi's changes to the checkout and builds everything. Both run
in the container from `Dockerfile` - see the top-level README for the commands.

`build.sh` passes its arguments on to `make`:

| Option | Effect |
|---|---|
| `PC=1` | The PC emulator (without it, the bring-up kernel) |
| `NOUART=1` | No serial log and no serial keyboard: the build for a card that is played on |
| `STATS=1` | Every ten seconds: CPU speed, audio queues, disk time, per-core work and sleep, MIDI voices |
| `JIT=1` | An experimental AArch64 block translator; off by default and not faster than the interpreter |
| `SBLOG=1` | Records the Sound Blaster's ports, DMA, interrupts and PCM for the `F4`/`F3` dumps |
| `OPLLOG=1` | Records the OPL's register writes and status reads for the `F9`/`F8` commands |

## The serial link

A development build logs on the PL011 UART (GPIO14/15, 115200 8N1) and reads commands
from it:

| Bytes | Meaning |
|---|---|
| `FE`, 1/0, keycode | A key pressed or released (Linux/AT keycodes, as the USB keyboard produces) |
| `F7` | Press the front panel's power button |
| `F6` | Press the front panel's reset button |
| `FA`, address (4, LE), length (2, LE) | Print that much guest memory |
| `FD` / `FC` / `FB` | Print / re-arm / freeze the guest exception ring |
| `F5` | Print the video card's registers and the text geometry derived from them |
| `F4` / `F3` | Print the Sound Blaster log / the last 32 KB of PCM it was given (`SBLOG=1`) |
| `F9` / `F8` | Start / print the OPL register log (`OPLLOG=1`) |

## Tools in `debug/`

| Tool | Use |
|---|---|
| `serial_load.py` | Loads an Intel HEX kernel (`aarch64-linux-gnu-objcopy -O ihex kernel8-pc.elf`) into Circle's serial RAM bootloader, so a new build runs without touching the card; `--reset-line rts` resets the board first when RTS is wired to the Pi's RUN pad |
| `pi_reset.py` | Pulses RTS or DTR to reset the board through RUN |
| `send_keys.py` | Types into the guest over the serial link |
| `uart_log.py` | Records the serial log, receive only |
| `live/serve.py` | Serves an HDMI capture device (via ffmpeg) as live video and single frames |
| `pi3-jtag.cfg`, `recover-core0.cfg` | OpenOCD configuration for a Pico-based JTAG probe on GPIO22-27 (`enable_jtag_gpio=1` in `config.txt`) |
| `libm_test.*`, `jit_encode_test.*` | Host checks of the maths library and the translator's instruction encoder |
