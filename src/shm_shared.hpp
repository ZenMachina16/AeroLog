#pragma once

#include <atomic>
#include <cstdint>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#define SHM_NAME  "/aerolog_buffer"
#define RING_SIZE (1u << 20)  // 1,048,576 slots — must be power of 2 for fast modulo

// ---------------------------------------------------------------------------
// Market event type discriminator.
// ---------------------------------------------------------------------------
enum class EventType : uint8_t {
    TRADE  = 0,
    QUOTE  = 1,
    CANCEL = 2,
};

// ---------------------------------------------------------------------------
// LogEntry — 56 bytes, fully explicit layout.
//
// Every field offset and size is deterministic across compilers so that the
// Python analyzer can decode the binary stream with a fixed struct format
// string without relying on compiler-dependent padding.
//
// Python format string: '=Q8sdddIB3xQ'
//   Q   (8)  timestamp_ns
//   8s  (8)  symbol
//   d   (8)  bid
//   d   (8)  ask
//   d   (8)  last_price
//   I   (4)  volume
//   B   (1)  event_type
//   3x  (3)  explicit padding (matches C++ pad[3])
//   Q   (8)  write_latency_ns
//             ─────────────────
//            56 bytes total
// ---------------------------------------------------------------------------
struct LogEntry {
    uint64_t timestamp_ns;      // Unix nanoseconds
    char     symbol[8];         // Null-padded instrument, e.g. "AAPL\0\0\0\0"
    double   bid;               // Best bid price
    double   ask;               // Best ask price
    double   last_price;        // Last traded / close price
    uint32_t volume;            // Quantity
    uint8_t  event_type;        // EventType enum value
    uint8_t  pad[3];            // Explicit alignment padding
    uint64_t write_latency_ns;  // Hot-path write cost measured by the producer
};

static_assert(sizeof(LogEntry) == 56, "LogEntry layout changed — update Python format string");

// ---------------------------------------------------------------------------
// SharedBuffer — SPSC ring in POSIX shared memory.
//
// head and tail each occupy one full 64-byte cache line to eliminate false
// sharing between the producer core and the consumer core.
// ---------------------------------------------------------------------------
struct SharedBuffer {
    std::atomic<uint64_t> head;
    char                  _head_pad[64 - sizeof(std::atomic<uint64_t>)];

    std::atomic<uint64_t> tail;
    char                  _tail_pad[64 - sizeof(std::atomic<uint64_t>)];

    // Producer sets this to true before exiting so the consumer can drain
    // the remaining entries and then shut down cleanly.
    std::atomic<bool>     producer_done;
    char                  _done_pad[64 - sizeof(std::atomic<bool>)];

    LogEntry entries[RING_SIZE];
};
