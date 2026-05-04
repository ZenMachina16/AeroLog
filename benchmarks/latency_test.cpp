#include "../src/shm_shared.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
constexpr int NUM_LOGS = 1'000'000;

// ---------------------------------------------------------------------------
// Benchmark 1 — Standard blocking file I/O (std::FILE text writes)
// ---------------------------------------------------------------------------
static void benchmark_standard() {
    std::cout << "[Standard I/O] Starting " << NUM_LOGS << " log writes...\n";

    FILE* f = fopen("standard.log", "w");
    if (!f) { perror("fopen"); return; }

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_LOGS; ++i)
        std::fprintf(f, "id=%d value=%.2f ts=%llu\n", i, i * 1.5, 123456789ULL);

    fclose(f);  // force flush

    auto end = std::chrono::high_resolution_clock::now();
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "[Standard I/O] Done in " << ms << " ms  (~"
              << (NUM_LOGS / (ms + 1)) * 1000 << " logs/s)\n";
}

// ---------------------------------------------------------------------------
// Benchmark 2 — AeroLog SPSC shared-memory ring buffer.
//
// A drain thread is spawned inside this function so the benchmark is
// self-contained and does not deadlock when the external consumer binary is
// not running.
// ---------------------------------------------------------------------------
static void benchmark_aerolog() {
    std::cout << "[AeroLog] Starting " << NUM_LOGS << " log writes...\n";

    // ── Setup SHM ─────────────────────────────────────────────────────────
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd == -1) { perror("shm_open"); return; }
    if (ftruncate(fd, sizeof(SharedBuffer)) == -1) { perror("ftruncate"); return; }

    auto* ring = static_cast<SharedBuffer*>(
        mmap(nullptr, sizeof(SharedBuffer),
             PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (ring == MAP_FAILED) { perror("mmap"); return; }
    close(fd);

    ring->head.store(0, std::memory_order_relaxed);
    ring->tail.store(0, std::memory_order_relaxed);
    ring->producer_done.store(false, std::memory_order_relaxed);

    // ── Drain thread — mirrors consumer logic, no disk I/O ────────────────
    std::thread drainer([ring]() {
        uint64_t drained = 0;
        while (true) {
            uint64_t h = ring->head.load(std::memory_order_relaxed);
            if (h != ring->tail.load(std::memory_order_acquire)) {
                ring->head.store((h + 1) & (RING_SIZE - 1),
                                 std::memory_order_release);
                drained++;
            } else if (ring->producer_done.load(std::memory_order_acquire)) {
                break;
            }
        }
    });

    // ── Producer hot path — timed section ─────────────────────────────────
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_LOGS; ++i) {
        uint64_t cur  = ring->tail.load(std::memory_order_relaxed);
        uint64_t next = (cur + 1) & (RING_SIZE - 1);

        while (next == ring->head.load(std::memory_order_acquire)) { /* spin */ }

        LogEntry& e   = ring->entries[cur];
        e.event_type  = static_cast<uint8_t>(EventType::TRADE);
        e.timestamp_ns = static_cast<uint64_t>(i) * 1'000ULL;
        e.last_price  = i * 1.5;
        e.volume      = static_cast<uint32_t>(i);
        std::memset(e.symbol, 0, sizeof(e.symbol));
        std::memcpy(e.symbol, "TEST", 4);  // buffer pre-zeroed
        e.bid = e.ask = e.last_price;
        std::memset(e.pad, 0, sizeof(e.pad));
        e.write_latency_ns = 0;

        ring->tail.store(next, std::memory_order_release);
    }

    ring->producer_done.store(true, std::memory_order_release);

    auto end = std::chrono::high_resolution_clock::now();
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    drainer.join();

    std::cout << "[AeroLog]      Done in " << ms << " ms  (~"
              << (NUM_LOGS / (ms + 1)) * 1000 << " logs/s)\n";

    munmap(ring, sizeof(SharedBuffer));
    shm_unlink(SHM_NAME);
}

// ---------------------------------------------------------------------------
// Percentile helper — collect per-entry write latency from a .alog file
// and print a report.
// ---------------------------------------------------------------------------
static void analyze_latency_from_log(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { return; }

    std::vector<uint64_t> lats;
    LogEntry e{};
    while (fread(&e, sizeof(LogEntry), 1, f) == 1) {
        if (e.write_latency_ns > 0)
            lats.push_back(e.write_latency_ns);
    }
    fclose(f);

    if (lats.empty()) return;
    std::sort(lats.begin(), lats.end());
    const size_t n = lats.size();
    auto pct = [&](double p) {
        return lats[static_cast<size_t>(p / 100.0 * n)];
    };

    std::cout << "\n[Latency from " << path << "]\n";
    std::cout << "  P50  : " << pct(50)   << " ns\n";
    std::cout << "  P99  : " << pct(99)   << " ns\n";
    std::cout << "  P99.9: " << pct(99.9) << " ns\n";
    std::cout << "  Max  : " << lats.back() << " ns\n";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    std::cout << "═══════════════════════════════════════════════════\n";
    std::cout << "  AeroLog Benchmark Suite — " << NUM_LOGS << " events\n";
    std::cout << "═══════════════════════════════════════════════════\n\n";

    benchmark_standard();
    std::cout << "\n";
    benchmark_aerolog();

    // Optionally read latency stats from a pre-existing telemetry log
    analyze_latency_from_log("../src/telemetry.alog");

    std::cout << "\n═══════════════════════════════════════════════════\n";
    return 0;
}
