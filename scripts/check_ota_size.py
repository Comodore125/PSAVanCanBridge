#!/usr/bin/env python3
"""CI: verify the *real* ESP32 image fits both OTA slots with a small safety margin."""
import csv
import pathlib
import sys

if len(sys.argv) != 3:
    raise SystemExit("usage: check_ota_size.py partitions.csv firmware.bin")
partition_path, firmware_path = map(pathlib.Path, sys.argv[1:])
slots = {}
with partition_path.open(encoding="utf-8") as stream:
    for row in csv.reader(line for line in stream if line.strip() and not line.lstrip().startswith("#")):
        if len(row) < 5:
            continue
        name, part_type, subtype, _, size = (x.strip() for x in row[:5])
        if part_type == "app" and subtype in ("ota_0", "ota_1"):
            slots[subtype] = int(size, 0)
assert set(slots) == {"ota_0", "ota_1"}, f"Missing OTA slot: {slots}"
firmware = firmware_path.read_bytes()
if not firmware or firmware[0] != 0xE9:
    raise SystemExit("FAIL: firmware.bin does not look like an ESP32 application image")
margin = 4096
for subtype, capacity in sorted(slots.items()):
    remaining = capacity - len(firmware)
    print(f"{subtype}: binary={len(firmware):,} B; capacity={capacity:,} B; remaining={remaining:,} B")
    if remaining < margin:
        raise SystemExit(f"FAIL: {subtype} lacks {margin} bytes of OTA safety margin")
print("PASS: actual firmware.bin fits both OTA partitions with >=4 KiB margin")
