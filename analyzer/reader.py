"""
AeroLog binary log decoder.

Usage:
    python3 reader.py [path/to/telemetry.alog]

Decodes the binary .alog format written by the C++ consumer and prints a
summary table to stdout.

Binary format per entry (56 bytes, little-endian):
    Q   8   timestamp_ns
    8s  8   symbol (null-padded)
    d   8   bid
    d   8   ask
    d   8   last_price
    I   4   volume
    B   1   event_type  (0=TRADE, 1=QUOTE, 2=CANCEL)
    3x  3   padding
    Q   8   write_latency_ns
"""

import os
import struct
import sys

# ---------------------------------------------------------------------------
# ABI constants — must match LogEntry in src/shm_shared.hpp
# ---------------------------------------------------------------------------
ENTRY_FMT  = struct.Struct("=Q8sdddIB3xQ")
ENTRY_SIZE = ENTRY_FMT.size          # 56 bytes
assert ENTRY_SIZE == 56, f"Unexpected entry size: {ENTRY_SIZE}"

EVENT_NAMES = {0: "TRADE", 1: "QUOTE", 2: "CANCEL"}


def decode_entry(raw: bytes) -> dict:
    ts, sym_b, bid, ask, last, vol, etype, lat = ENTRY_FMT.unpack(raw)
    return {
        "timestamp_ns":      ts,
        "symbol":            sym_b.rstrip(b"\x00").decode("ascii", errors="replace"),
        "bid":               bid,
        "ask":               ask,
        "last_price":        last,
        "volume":            vol,
        "event_type":        EVENT_NAMES.get(etype, f"UNK({etype})"),
        "write_latency_ns":  lat,
    }


def read_log(path: str, preview: int = 5) -> list[dict]:
    if not os.path.exists(path):
        print(f"Error: file not found — {path}")
        print("  Did you run the C++ consumer?  (it creates the .alog file)")
        sys.exit(1)

    size = os.path.getsize(path)
    total = size // ENTRY_SIZE
    print(f"Log file  : {path}")
    print(f"File size : {size:,} bytes  ({total:,} entries × {ENTRY_SIZE} bytes)")
    print()

    entries = []
    with open(path, "rb") as f:
        while True:
            raw = f.read(ENTRY_SIZE)
            if len(raw) < ENTRY_SIZE:
                break
            entries.append(decode_entry(raw))

    if not entries:
        print("No entries found.")
        return entries

    # Print header + first/last preview rows
    hdr = f"{'#':>7}  {'Symbol':<8}  {'Type':<7}  {'Last':>10}  "
    hdr += f"{'Bid':>10}  {'Ask':>10}  {'Volume':>8}  {'Latency(ns)':>12}"
    sep = "─" * len(hdr)

    print(f"{'First ' + str(preview) + ' entries':}")
    print(sep)
    print(hdr)
    print(sep)
    for i, e in enumerate(entries[:preview]):
        _print_row(i, e)

    if len(entries) > preview * 2:
        print(f"  ... ({len(entries) - preview * 2} entries omitted) ...")

    print(sep)
    print(f"Last {preview} entries")
    print(sep)
    print(hdr)
    print(sep)
    for i, e in enumerate(entries[-preview:]):
        _print_row(len(entries) - preview + i, e)
    print(sep)

    # Latency summary
    lats = [e["write_latency_ns"] for e in entries if e["write_latency_ns"] > 0]
    if lats:
        lats.sort()
        n = len(lats)
        def pct(p):
            return lats[int(p / 100 * n)]
        print(f"\nWrite latency ({n} entries with data):")
        print(f"  P50  : {pct(50):>8,} ns")
        print(f"  P90  : {pct(90):>8,} ns")
        print(f"  P99  : {pct(99):>8,} ns")
        print(f"  P99.9: {pct(99.9):>8,} ns")
        print(f"  Max  : {lats[-1]:>8,} ns")

    print(f"\n[Done] {len(entries):,} total entries decoded.")
    return entries


def _print_row(idx: int, e: dict) -> None:
    print(
        f"{idx:>7}  {e['symbol']:<8}  {e['event_type']:<7}  "
        f"{e['last_price']:>10.4f}  {e['bid']:>10.4f}  {e['ask']:>10.4f}  "
        f"{e['volume']:>8}  {e['write_latency_ns']:>12,}"
    )


if __name__ == "__main__":
    log_path = sys.argv[1] if len(sys.argv) > 1 else "../src/telemetry.alog"
    read_log(log_path)
