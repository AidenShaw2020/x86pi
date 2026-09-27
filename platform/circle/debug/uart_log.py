"""Bounded, RX-only UART capture. Never toggles DTR/RTS or sends data."""
import argparse
import pathlib
import time
import serial

p = argparse.ArgumentParser()
p.add_argument("port")
p.add_argument("output", type=pathlib.Path)
p.add_argument("--seconds", type=float, default=120)
a = p.parse_args()
if a.seconds <= 0:
    p.error("seconds must be positive")
with a.output.open("xb") as log:
    uart = serial.Serial(port=None, baudrate=115200, timeout=0.25)
    uart.dtr = False
    uart.rts = False
    uart.port = a.port
    uart.open()
    try:
        print(f"RX-only {a.port} 115200 8N1 -> {a.output}", flush=True)
        end = time.monotonic() + a.seconds
        while time.monotonic() < end:
            data = uart.read(4096)
            if data:
                log.write(data)
                log.flush()
                print(data.decode("utf-8", errors="replace"), end="", flush=True)
    finally:
        uart.close()
