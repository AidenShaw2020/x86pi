"""Pulse a USB-UART modem-control line wired to the Pi's RUN pad.

RUN is an active-low reset with its own pull-up on the board, so a reset means
driving the line low briefly and then releasing it.  Wire it through a series
resistor (about 1k is plenty): the resistor means the adapter and the board's
pull-up can never fight each other, and a wrong polarity cannot source or sink
more than a couple of milliamps.

Nothing here assumes a polarity.  pyserial's `rts`/`dtr` properties mean
"assert this signal", and whether an asserted signal is a high or a low at the
pin depends on the adapter, so --invert exists to swap it once measured.

The port is opened with both lines already de-asserted, the same way
serial_load.py and uart_log.py do it, so merely opening the port cannot pulse
the board.
"""
import argparse
import time
import serial

p = argparse.ArgumentParser()
p.add_argument("port")
p.add_argument("--line", choices=("rts", "dtr"), default="rts")
p.add_argument("--ms", type=float, default=100.0,
               help="How long to hold the reset asserted")
p.add_argument("--invert", action="store_true",
               help="Use when the adapter's asserted state is the released one")
a = p.parse_args()

held = not a.invert
uart = serial.Serial(port=None, baudrate=115200, timeout=0.1)
uart.rts = False
uart.dtr = False
uart.port = a.port
uart.open()
try:
    setattr(uart, a.line, held)
    time.sleep(a.ms / 1000.0)
    setattr(uart, a.line, not held)
    print(f"pulsed {a.line} on {a.port} for {a.ms:.0f} ms "
          f"(asserted={held}); board should be out of reset now")
finally:
    uart.close()
