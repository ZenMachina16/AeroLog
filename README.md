# AeroLog: Zero-Latency HFT Telemetry System
[![Build & Test](https://github.com/ZenMachina16/AeroLog/actions/workflows/ci.yml/badge.svg)](https://github.com/ZenMachina16/AeroLog/actions/workflows/ci.yml)


A high-throughput, lock-free IPC logging library for latency-sensitive trading applications.

## Overview

AeroLog is a C++ telemetry engine designed for High-Frequency Trading (HFT) environments where
every microsecond counts. Unlike standard logging libraries that block the main execution thread
to write to disk, AeroLog uses a **Single-Producer Single-Consumer (SPSC) lock-free ring buffer**
in POSIX Shared Memory (`/dev/shm`).

This architecture decouples the **Hot Path** (Trading Strategy) from the **Cold Path** (Disk I/O),
allowing the main application to log data in nanoseconds while a separate **Sidecar** process
handles disk persistence asynchronously.

## Key Features

| Feature | Detail |
| :--- | :--- |
| **Zero-Latency Hot Path** | Writes to shared memory in <100 ns (P99), no syscall on the critical path |
| **Lock-Free Architecture** | `std::atomic` acquire/release semantics — no mutexes, no priority inversion |
| **Cache-Line Isolation** | `head` and `tail` atomics are on separate 64-byte cache lines to eliminate false sharing |
| **Zero-Copy IPC** | Producer and consumer share the same physical RAM pages via `mmap` |
| **Graceful Shutdown** | `SIGINT`/`SIGTERM` handlers flush in-flight data; `producer_done` flag drives clean consumer exit |
| **Self-Reporting Latency** | Every log entry carries its own `write_latency_ns`; consumer prints P50/P99/P99.9 on exit |
| **CPU Affinity** | Optional `ENABLE_CPU_AFFINITY` pins producer to core 2 and consumer to core 3 |
| **Market Replay Mode** | Simulates a trading engine replaying historical OHLCV CSV data at full throttle |
| **Interactive Dashboard** | Plotly HTML dashboard: price chart, volume, and latency histogram — no server needed |

## Performance Benchmarks

1,000,000 financial events written on a commodity Linux machine:

| Method | Total Time | Throughput | Notes |
| :--- | :--- | :--- | :--- |
| `std::FILE` text I/O | ~500 ms | ~2 M events/s | Blocked by disk I/O |
| AeroLog SPSC ring | ~30 ms | ~34 M events/s | Non-blocking, CPU-bound |

**AeroLog is ~17× faster** because the trading thread only touches shared DRAM — zero kernel
transitions and zero disk waits on the hot path.

### Observed Write Latency (P99 < 50 ns)

```
--- Hot-Path Write Latency Report (1,000,000 events) ---
  P50  : 18 ns
  P90  : 24 ns
  P99  : 41 ns
  P99.9: 312 ns
  Max  : 8,420 ns
```

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      Trading Engine (Producer)               │
│                                                             │
│   Parse CSV  ──►  fill LogEntry  ──►  atomic tail.store()  │
│                        ~20 CPU cycles, no syscall           │
└─────────────────────┬───────────────────────────────────────┘
                      │  /dev/shm  (mmap, same physical pages)
┌─────────────────────▼───────────────────────────────────────┐
│             SharedBuffer (SPSC Ring, 33 MB)                  │
│                                                             │
│  [head ─ cacheline 0]   [tail ─ cacheline 1]               │
│  [producer_done]        [entries[1,048,576]]                │
└─────────────────────┬───────────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────────────────┐
│                   Sidecar Consumer                           │
│                                                             │
│  Poll head≠tail  ──►  batch  ──►  fwrite()  ──►  .alog    │
│  On exit: P50/P99/P99.9 latency report                     │
└─────────────────────────────────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────────────────┐
│                 Python Analyzer                              │
│                                                             │
│  reader.py   — decode binary .alog → terminal table        │
│  dashboard.py — Plotly interactive HTML (price/vol/latency) │
└─────────────────────────────────────────────────────────────┘
```

## Binary Log Format

Every `LogEntry` is **56 bytes**, with fully explicit layout (no implicit padding):

```
Offset  Size  Field              Python struct token
──────  ────  ─────────────────  ───────────────────
  0       8   timestamp_ns       Q
  8       8   symbol[8]          8s
 16       8   bid                d
 24       8   ask                d
 32       8   last_price         d
 40       4   volume             I
 44       1   event_type         B   (0=TRADE 1=QUOTE 2=CANCEL)
 45       3   pad[3]             3x
 48       8   write_latency_ns   Q
──────  ────
Total  56
```

Python format string: `'=Q8sdddIB3xQ'`

## Build

### Prerequisites

- Linux (Ubuntu 20.04+ or WSL2)
- g++ with C++17 support
- CMake ≥ 3.16
- Python 3.10+ (for the analyzer)

### Compile

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Optional — pin producer/consumer to dedicated CPU cores:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_CPU_AFFINITY=ON
cmake --build build --parallel
```

Binaries are written to `build/`: `producer`, `consumer`, `latency_test`.

### Run the Tests

```bash
cd build && ctest --output-on-failure
```

## Usage

### 1. Start the Sidecar (Consumer)

The consumer waits for the producer's shared memory region to appear, so it can
be started before or after the producer — up to a 10-second window.

```bash
./build/consumer                          # default: writes telemetry.alog
./build/consumer /tmp/trades.alog         # custom output path
./build/consumer /tmp/trades.alog 2000    # custom output + batch size
```

### 2. Run the Trading Engine (Producer)

```bash
./build/producer                          # default: reads ../trades.csv
./build/producer /path/to/my_data.csv     # custom CSV path
```

Expected output:

```
AeroLog Producer
  CSV source  : ../trades.csv
  SHM name    : /aerolog_buffer
  Ring size   : 1048576 slots

Producer: created shared memory buffer
Producer: replaying market data...

Producer: finished. Replayed 10000 events in 12 ms

--- Producer Latency Report (10000 events) ---
  P50  : 21 ns
  P90  : 28 ns
  P99  : 47 ns
  P99.9: 390 ns
  Max  : 6820 ns
----------------------------------------------
```

### 3. Analyze the Binary Log

**Decode to terminal:**

```bash
python3 analyzer/reader.py build/telemetry.alog
```

**Generate interactive HTML dashboard:**

```bash
pip install -r requirements.txt
python3 analyzer/dashboard.py build/telemetry.alog dashboard.html
# Open dashboard.html in any browser
```

The dashboard contains three linked panels:

- **Price chart** — bid, ask, and last_price over time
- **Volume bars** — trade volume per event
- **Latency histogram** — write latency distribution with P50/P99 markers

### 4. Run the Benchmark

The benchmark is fully self-contained — it spawns its own drain thread so it
will not deadlock even without an external consumer:

```bash
./build/latency_test
```

## Project Structure

```
AeroLog/
├── src/
│   ├── shm_shared.hpp      # LogEntry + SharedBuffer (SPSC ring) — shared ABI
│   ├── producer.cpp        # Trading engine hot path
│   └── consumer.cpp        # Logging sidecar (cold path)
├── benchmarks/
│   └── latency_test.cpp    # Self-contained performance comparison suite
├── tests/
│   └── ring_buffer_test.cpp # SPSC correctness + ABI unit tests (ctest)
├── analyzer/
│   ├── reader.py           # Binary .alog decoder → terminal table
│   └── dashboard.py        # Plotly interactive HTML dashboard
├── .github/
│   └── workflows/
│       └── ci.yml          # GitHub Actions: build Release+Debug, ctest, flake8
├── CMakeLists.txt          # CMake build system
├── requirements.txt        # Python dependencies
└── trades.csv              # Historical market data (not tracked in git)
```

> **Note:** `trades.csv` is excluded from version control (see `.gitignore`).
> Place your own OHLCV CSV file in the project root or pass the path as `argv[1]`
> to the producer.  Expected columns: `Timestamp,Open,High,Low,Close[,Volume]`.
