#include "shm_shared.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <csignal>
#include <time.h>

#ifdef ENABLE_CPU_AFFINITY
#include <sched.h>
static void pin_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) != 0) {
        std::cerr << "Warning: Could not pin to CPU core " << core_id
                  << " (try running with more cores or disable ENABLE_CPU_AFFINITY)\n";
    } else {
        std::cout << "Producer: pinned to CPU core " << core_id << "\n";
    }
}
#endif

// ---------------------------------------------------------------------------
// Global shutdown flag — set by SIGINT / SIGTERM handler.
// ---------------------------------------------------------------------------
static std::atomic<bool> g_shutdown{false};

static void on_signal(int) {
    g_shutdown.store(true, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Nanosecond timestamp using CLOCK_MONOTONIC (vDSO, ~20 ns overhead).
// ---------------------------------------------------------------------------
static inline uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
}

// ---------------------------------------------------------------------------
// Print percentile table from a latency vector (already sorted).
// ---------------------------------------------------------------------------
static void print_latency_report(std::vector<uint64_t>& lat, uint64_t wall_ms) {
    if (lat.empty()) return;
    std::sort(lat.begin(), lat.end());
    const size_t n = lat.size();

    auto pct = [&](double p) -> uint64_t {
        size_t idx = static_cast<size_t>(p / 100.0 * static_cast<double>(n));
        if (idx >= n) idx = n - 1;
        return lat[idx];
    };

    std::cout << "\n--- Producer Latency Report (" << n << " events) ---\n";
    std::cout << "  P50  : " << pct(50.0)  << " ns\n";
    std::cout << "  P90  : " << pct(90.0)  << " ns\n";
    std::cout << "  P99  : " << pct(99.0)  << " ns\n";
    std::cout << "  P99.9: " << pct(99.9)  << " ns\n";
    std::cout << "  Max  : " << lat.back()  << " ns\n";
    if (wall_ms > 0) {
        std::cout << "  Throughput: "
                  << (n * 1000ULL / wall_ms) << " events/s\n";
    }
    std::cout << "---------------------------------------------\n";
}

int main(int argc, char* argv[]) {
    // ---- Parse CLI arguments -------------------------------------------------
    std::string csv_path = "../trades.csv";
    if (argc >= 2) csv_path = argv[1];

    std::cout << "AeroLog Producer\n";
    std::cout << "  CSV source  : " << csv_path << "\n";
    std::cout << "  SHM name    : " << SHM_NAME << "\n";
    std::cout << "  Ring size   : " << RING_SIZE << " slots\n\n";

    // ---- CPU affinity --------------------------------------------------------
#ifdef ENABLE_CPU_AFFINITY
    pin_to_core(2);
#endif

    // ---- Signal handlers -----------------------------------------------------
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    // ---- Create / attach shared memory ---------------------------------------
    // Try exclusive create first so we only zero-initialize when we own it.
    int fd = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0666);
    bool is_creator = (fd != -1);

    if (!is_creator) {
        fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (fd == -1) { perror("shm_open failed"); return 1; }
    }

    if (ftruncate(fd, sizeof(SharedBuffer)) == -1) {
        perror("ftruncate failed"); return 1;
    }

    auto* ring = static_cast<SharedBuffer*>(
        mmap(nullptr, sizeof(SharedBuffer),
             PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (ring == MAP_FAILED) { perror("mmap failed"); return 1; }
    close(fd);

    if (is_creator) {
        ring->head.store(0, std::memory_order_relaxed);
        ring->tail.store(0, std::memory_order_relaxed);
        ring->producer_done.store(false, std::memory_order_relaxed);
        std::cout << "Producer: created shared memory buffer\n";
    } else {
        ring->producer_done.store(false, std::memory_order_relaxed);
        std::cout << "Producer: attached to existing shared memory buffer\n";
    }

    // ---- Open CSV file -------------------------------------------------------
    std::ifstream file(csv_path);
    if (!file.is_open()) {
        std::cerr << "Error: could not open CSV: " << csv_path << "\n";
        std::cerr << "  Place trades.csv in the project root, or pass the path as argv[1]\n";
        shm_unlink(SHM_NAME);
        return 1;
    }

    // Skip header row (e.g. "Timestamp,Open,High,Low,Close,Volume")
    std::string line;
    std::getline(file, line);

    // ---- Pre-allocate latency collection ------------------------------------
    std::vector<uint64_t> latencies;
    latencies.reserve(1 << 20);  // 1M pre-allocated; grows if CSV is larger

    int count = 0;
    auto wall_start = std::chrono::steady_clock::now();

    std::cout << "Producer: replaying market data...\n";

    // ---- Main send loop ------------------------------------------------------
    while (std::getline(file, line) && !g_shutdown.load(std::memory_order_relaxed)) {
        std::stringstream ss(line);
        std::string seg;
        std::vector<std::string> row;

        while (std::getline(ss, seg, ','))
            row.push_back(seg);

        // Expect at least: Timestamp(0), Open(1), High(2), Low(3), Close(4)
        if (row.size() < 5) continue;

        try {
            // Parse fields — timestamp may carry a trailing ".0" fractional part
            auto ts_sec    = static_cast<uint64_t>(std::stod(row[0]));
            double open_p  = std::stod(row[1]);
            double high_p  = std::stod(row[2]);
            // row[3] = low (unused in entry but available)
            double close_p = std::stod(row[4]);
            uint32_t vol   = (row.size() >= 6) ? static_cast<uint32_t>(std::stod(row[5])) : 0;

            // ── Acquire a slot ────────────────────────────────────────────────
            uint64_t current_tail = ring->tail.load(std::memory_order_relaxed);
            uint64_t next_tail    = (current_tail + 1) & (RING_SIZE - 1);

            // Measure total hot-path latency: spin-wait + fill + commit
            uint64_t t_start = now_ns();

            // Backpressure: spin until consumer frees a slot
            while (next_tail == ring->head.load(std::memory_order_acquire)) {
                if (g_shutdown.load(std::memory_order_relaxed)) goto cleanup;
            }

            // ── Fill the entry (before commit so no race with consumer) ───────
            {
                LogEntry& entry   = ring->entries[current_tail];
                entry.timestamp_ns  = ts_sec * 1'000'000'000ULL;
                std::memset(entry.symbol, 0, sizeof(entry.symbol));
                std::memcpy(entry.symbol, "MARKET", 6);  // buffer pre-zeroed
                entry.bid           = open_p;
                entry.ask           = high_p;
                entry.last_price    = close_p;
                entry.volume        = vol;
                entry.event_type    = static_cast<uint8_t>(EventType::TRADE);
                std::memset(entry.pad, 0, sizeof(entry.pad));
                entry.write_latency_ns = 0;  // filled in below before store

                uint64_t t_fill = now_ns();
                entry.write_latency_ns = t_fill - t_start;

                // ── Commit the write (visible to consumer only after this) ────
                ring->tail.store(next_tail, std::memory_order_release);
            }

            latencies.push_back(now_ns() - t_start);
            count++;

        } catch (...) {
            continue;
        }
    }

cleanup:
    // ---- Signal consumer that all data is enqueued --------------------------
    ring->producer_done.store(true, std::memory_order_release);

    auto wall_end = std::chrono::steady_clock::now();
    uint64_t wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           wall_end - wall_start).count();

    std::cout << "\nProducer: finished. Replayed " << count << " events in "
              << wall_ms << " ms\n";

    if (g_shutdown.load())
        std::cout << "Producer: received shutdown signal\n";

    print_latency_report(latencies, wall_ms);

    munmap(ring, sizeof(SharedBuffer));
    return 0;
}
