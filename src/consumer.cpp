#include "shm_shared.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <csignal>

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
        std::cout << "Consumer: pinned to CPU core " << core_id << "\n";
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
// Print a percentile latency table.
// ---------------------------------------------------------------------------
static void print_latency_report(std::vector<uint64_t>& lat, uint64_t wall_ms) {
    if (lat.empty()) {
        std::cout << "No latency data collected.\n";
        return;
    }
    std::sort(lat.begin(), lat.end());
    const size_t n = lat.size();

    auto pct = [&](double p) -> uint64_t {
        size_t idx = static_cast<size_t>(p / 100.0 * static_cast<double>(n));
        if (idx >= n) idx = n - 1;
        return lat[idx];
    };

    std::cout << "\n╔══════════════════════════════════════════╗\n";
    std::cout << "║   Hot-Path Write Latency Report          ║\n";
    std::cout << "╠══════════════════════════════════════════╣\n";
    std::cout << "║  Events  : " << n << "\n";
    std::cout << "║  P50     : " << pct(50.0)  << " ns\n";
    std::cout << "║  P90     : " << pct(90.0)  << " ns\n";
    std::cout << "║  P99     : " << pct(99.0)  << " ns\n";
    std::cout << "║  P99.9   : " << pct(99.9)  << " ns\n";
    std::cout << "║  Max     : " << lat.back()  << " ns\n";
    if (wall_ms > 0) {
        std::cout << "║  Throughput: "
                  << (n * 1000ULL / wall_ms) << " events/s\n";
    }
    std::cout << "╚══════════════════════════════════════════╝\n";
}

int main(int argc, char* argv[]) {
    // ---- Parse CLI arguments -------------------------------------------------
    std::string output_path = "telemetry.alog";
    int         batch_size  = 1000;

    if (argc >= 2) output_path = argv[1];
    if (argc >= 3) batch_size  = std::stoi(argv[2]);

    std::cout << "AeroLog Consumer (Sidecar)\n";
    std::cout << "  Output      : " << output_path << "\n";
    std::cout << "  Batch size  : " << batch_size << " entries\n";
    std::cout << "  SHM name    : " << SHM_NAME << "\n\n";

    // ---- CPU affinity --------------------------------------------------------
#ifdef ENABLE_CPU_AFFINITY
    pin_to_core(3);
#endif

    // ---- Signal handlers -----------------------------------------------------
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    // ---- Wait for producer to create the shared memory -----------------------
    int fd = -1;
    for (int attempt = 0; attempt < 10 && fd == -1; ++attempt) {
        fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (fd == -1) {
            if (attempt == 0)
                std::cout << "Consumer: waiting for producer to create SHM";
            else
                std::cout << '.';
            std::cout.flush();
            sleep(1);
        }
    }
    if (fd == -1) {
        std::cerr << "\nConsumer: timed out waiting for SHM — did you start the producer?\n";
        return 1;
    }
    std::cout << "\nConsumer: attached to shared memory buffer\n";

    auto* ring = static_cast<SharedBuffer*>(
        mmap(nullptr, sizeof(SharedBuffer),
             PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (ring == MAP_FAILED) { perror("mmap failed"); return 1; }
    close(fd);

    // ---- Open output file ----------------------------------------------------
    FILE* fp = fopen(output_path.c_str(), "wb");
    if (!fp) {
        perror("fopen failed");
        munmap(ring, sizeof(SharedBuffer));
        return 1;
    }

    // ---- Pre-allocate local batch buffer + latency collection ---------------
    std::vector<LogEntry> disk_buffer;
    disk_buffer.reserve(static_cast<size_t>(batch_size));

    std::vector<uint64_t> latencies;
    latencies.reserve(1 << 20);

    uint64_t processed = 0;
    std::cout << "Consumer: listening for market data (batching every "
              << batch_size << " entries)...\n";

    auto wall_start = std::chrono::steady_clock::now();

    // ---- Main drain loop -----------------------------------------------------
    // Exit when the producer signals done AND we have drained every entry.
    while (!g_shutdown.load(std::memory_order_relaxed)) {
        uint64_t current_head = ring->head.load(std::memory_order_relaxed);

        if (current_head != ring->tail.load(std::memory_order_acquire)) {
            // ── Read entry from shared memory ──────────────────────────────
            LogEntry entry = ring->entries[current_head];  // copy before advancing head

            // ── Advance head (release so producer sees freed slot) ─────────
            ring->head.store((current_head + 1) & (RING_SIZE - 1),
                             std::memory_order_release);

            // ── Accumulate ────────────────────────────────────────────────
            disk_buffer.push_back(entry);
            if (entry.write_latency_ns > 0)
                latencies.push_back(entry.write_latency_ns);

            processed++;

            // ── Batch flush to disk ────────────────────────────────────────
            if (static_cast<int>(disk_buffer.size()) >= batch_size) {
                fwrite(disk_buffer.data(), sizeof(LogEntry),
                       disk_buffer.size(), fp);
                disk_buffer.clear();
            }
        } else {
            // Ring is empty — check if producer is finished
            if (ring->producer_done.load(std::memory_order_acquire))
                break;
            // Yield to avoid burning 100% CPU while idle
            // (remove in ultra-low-latency deployments and use busy-spin)
            sched_yield();
        }
    }

    // ---- Flush remaining entries to disk ------------------------------------
    if (!disk_buffer.empty()) {
        fwrite(disk_buffer.data(), sizeof(LogEntry), disk_buffer.size(), fp);
        disk_buffer.clear();
    }
    fclose(fp);

    auto wall_end = std::chrono::steady_clock::now();
    uint64_t wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           wall_end - wall_start).count();

    // ---- Final summary -------------------------------------------------------
    if (g_shutdown.load())
        std::cout << "\nConsumer: received shutdown signal — flushed partial data\n";

    std::cout << "\nConsumer: finished.\n";
    std::cout << "  Total processed : " << processed << " events\n";
    std::cout << "  Wall time       : " << wall_ms << " ms\n";
    std::cout << "  Output file     : " << output_path << "\n";

    print_latency_report(latencies, wall_ms);

    // ── Clean up shared memory ──────────────────────────────────────────────
    munmap(ring, sizeof(SharedBuffer));
    shm_unlink(SHM_NAME);

    return 0;
}
