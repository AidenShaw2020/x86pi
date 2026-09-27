"""Offline safety checks for the RAM upload HEX validator."""
import pathlib
import tempfile
import unittest
from serial_load import records

def ihex(kind, data=b"", addr=0):
    r = bytes([len(data), addr >> 8, addr & 255, kind]) + data
    return ":" + (r + bytes([-sum(r) & 255])).hex().upper()

class HexValidation(unittest.TestCase):
    def parse(self, lines):
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d) / "input.hex"
            p.write_text("\n".join(lines), encoding="ascii")
            return records(p)

    def valid(self):
        return [ihex(2, b"\x80\x00"), ihex(0, b"\x01\x02"),
                ihex(3, b"\x80\x00\x00\x00"), ihex(1)]

    def test_valid_segment_entry(self):
        self.assertEqual(len(self.parse(self.valid())), 4)

    def test_valid_linear_entry(self):
        self.assertEqual(len(self.parse([ihex(4,b"\x00\x08"),
            ihex(0,b"\x11"), ihex(5,b"\x00\x08\x00\x00"),ihex(1)])),4)

    def test_bad_checksum(self):
        r = self.valid(); r[1] = r[1][:-2] + "FF"
        with self.assertRaises(ValueError): self.parse(r)

    def test_low_address(self):
        with self.assertRaises(ValueError): self.parse([ihex(0,b"\x00"),ihex(1)])

    def test_bootloader_overlap(self):
        with self.assertRaises(ValueError):
            self.parse([ihex(4,b"\x00\x80"),ihex(0,b"\x00"),ihex(1)])

    def test_missing_eof(self):
        with self.assertRaises(ValueError): self.parse(self.valid()[:-1])

    def test_trailing_record(self):
        with self.assertRaises(ValueError): self.parse(self.valid()+[ihex(1)])

    def test_wrong_entry(self):
        r = self.valid(); r[2] = ihex(5,b"\x00\x09\x00\x00")
        with self.assertRaises(ValueError): self.parse(r)

if __name__ == "__main__": unittest.main()
