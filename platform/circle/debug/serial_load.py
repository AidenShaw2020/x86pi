"""Load an Intel HEX kernel into the pinned Circle RAM bootloader.

No SD writes. Refuse invalid checksums/addresses and never send go without EOF ack.
"""
import argparse
import pathlib
import time
import serial

def records(path):
    result, base, eof = [], 0, False
    for line in path.read_text(encoding="ascii").splitlines():
        if not line.startswith(":") or eof:
            raise ValueError("Invalid HEX record/order")
        rec = bytes.fromhex(line[1:])
        if len(rec) < 5 or len(rec) != rec[0] + 5 or sum(rec) & 255:
            raise ValueError("Invalid HEX length/checksum")
        kind = rec[3]
        if kind in (2, 4):
            if rec[0] != 2:
                raise ValueError("Invalid address record")
            base = int.from_bytes(rec[4:6], "big") << (4 if kind == 2 else 16)
        elif kind == 0:
            addr = base + int.from_bytes(rec[1:3], "big")
            if not rec[0] or addr < 0x80000 or addr + rec[0] > 0x800000:
                raise ValueError("HEX writes outside kernel RAM / into bootloader")
        elif kind == 1:
            if rec[0] != 0:
                raise ValueError("Invalid EOF")
            eof = True
        elif kind in (3, 5):
            if rec[0] != 4:
                raise ValueError("Invalid entry record")
            entry = (int.from_bytes(rec[4:6], "big") * 16 + int.from_bytes(rec[6:8], "big")
                     if kind == 3 else int.from_bytes(rec[4:8], "big"))
            if entry != 0x80000:
                raise ValueError("Unexpected kernel entry")
        else:
            raise ValueError("Unsupported HEX record")
        result.append(rec)
    if not eof:
        raise ValueError("Missing EOF")
    return result

def main():
    p = argparse.ArgumentParser()
    p.add_argument("hex", type=pathlib.Path)
    p.add_argument("port")
    p.add_argument("log", type=pathlib.Path)
    p.add_argument("--restart-ready", action="store_true",
                   help="Send R immediately; only use when the RAM bootloader is already running")
    p.add_argument("--reset-line", choices=("rts", "dtr"), default=None,
                   help="Pulse this modem-control line to reset the board before "
                        "waiting for the bootloader. Requires it to be wired to the "
                        "Pi's RUN pad; see pi_reset.py.")
    p.add_argument("--reset-invert", action="store_true",
                   help="Use when the adapter's asserted state is the released one")
    p.add_argument("--wait-seconds", type=float, default=180,
                   help="How long to wait for the bootloader's IHEX-F banner before "
                        "giving up. Raise it when the board will be power-cycled by "
                        "hand some time later.")
    p.add_argument("--watch-seconds", type=float, default=30,
                   help="How long to keep logging after the kernel is started. The default "
                        "matches the original behaviour; raise it to catch a fault that "
                        "happens later, with no gap in which the port is closed.")
    a = p.parse_args()
    payload = records(a.hex)
    with a.log.open("xb") as log:
        uart = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=10)
        uart.dtr = uart.rts = False
        uart.port = a.port
        uart.open()
        def read():
            data = uart.read(max(1, uart.in_waiting))
            if data:
                log.write(data)
                log.flush()
                print(data.decode("utf-8", errors="replace"), end="", flush=True)
            return data
        def wait_for(marker, seconds):
            response = b""
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                response += read()
                if b"#ERR:" in response:
                    raise RuntimeError("Bootloader rejected transfer; NOT starting kernel")
                if marker in response:
                    return
            raise TimeoutError(f"No {marker!r}; kernel NOT started")
        try:
            if a.reset_line:
                held = not a.reset_invert
                setattr(uart, a.reset_line, held)
                time.sleep(0.1)
                setattr(uart, a.reset_line, not held)
                print(f"pulsed {a.reset_line} to reset the board", flush=True)
            print(f"Waiting for Circle RAM bootloader ({a.wait_seconds:.0f} s)", flush=True)
            if a.restart_ready:
                uart.write(b"R")
                uart.flush()
            wait_for(b"IHEX-F", a.wait_seconds)
            uart.write(b"R")
            uart.flush()
            wait_for(b"IHEX-F", 5)  # verifies host TX -> Pi RX, not just logging
            wire = b"".join(b"=" + r for r in payload)
            started = time.monotonic()
            response = b""
            next_progress = 0
            for offset in range(0, len(wire), 2048):
                uart.write(wire[offset:offset+2048])
                uart.flush()
                if uart.in_waiting:
                    response += read()
                if b"#ERR:" in response:
                    raise RuntimeError("Bootloader checksum/format failure; NOT starting")
                percent = (offset + 2048) * 100 // len(wire)
                if percent >= next_progress:
                    print(f"UART upload {min(percent,100)}%", flush=True)
                    next_progress += 25
            if b"#EOF:ok" not in response:
                wait_for(b"#EOF:ok", 5)
            print(f"Checksum-acknowledged transfer in {time.monotonic()-started:.1f}s; starting", flush=True)
            uart.write(b"g")
            uart.flush()
            deadline = time.monotonic() + a.watch_seconds
            while time.monotonic() < deadline:
                read()
        finally:
            uart.close()

if __name__ == "__main__":
    main()
