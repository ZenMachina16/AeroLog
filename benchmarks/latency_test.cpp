#include <iostream>
#include <fstream>
#include <chrono>
#include <vector>
#include <cstring>
#include "../src/shm_shared.hpp" // Import your shared memory logic

// Settings
const int NUM_LOGS = 1000000;

// --- COMPETITOR 1: Standard File I/O ---
void benchmark_standard() {
    std::cout << "[Standard I/O] Starting..." << std::endl;
    
    // Open a standard file
    std::ofstream fs("standard.log");
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < NUM_LOGS; ++i) {
        // The "Slow" way: Formatting text and writing to disk buffer
        fs << "Log Entry ID:" << i << " Value:" << (i * 1.5) << " Time:" << 123456789 << "\n";
    }
    
    fs.close(); // Force flush to disk
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    std::cout << "[Standard I/O] Done. Time taken: " << duration << " ms" << std::endl;
}

// --- COMPETITOR 2: AeroLog (Your System) ---
void benchmark_aerolog() {
    std::cout << "[AeroLog] Starting..." << std::endl;

    // Connect to Shared Memory
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    ftruncate(fd, sizeof(SharedBuffer));
    SharedBuffer* ring = (SharedBuffer*)mmap(0, sizeof(SharedBuffer), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    // Reset for the test
    ring->head.store(0);
    ring->tail.store(0);

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_LOGS; ++i) {
        uint64_t current_tail = ring->tail.load(std::memory_order_relaxed);
        uint64_t next_tail = (current_tail + 1) % RING_SIZE;

        // SPINLOCK: If buffer is full, we wait. 
        // (This measures the true system throughput including backpressure)
        while (next_tail == ring->head.load(std::memory_order_acquire)) {
            // In a real benchmark, the Consumer should be running to clear this!
        }

        LogEntry& entry = ring->entries[current_tail];
        entry.event_id = i;
        entry.value = i * 1.5;
        entry.timestamp = 123456789;

        ring->tail.store(next_tail, std::memory_order_release);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "[AeroLog] Done. Time taken: " << duration << " ms" << std::endl;
}

int main() {
    std::cout << "--- BENCHMARK SUITE: 1 Million Logs ---\n" << std::endl;

    // Run Standard Test
    benchmark_standard();
    
    std::cout << "\n--------------------------------------\n" << std::endl;

    // Run AeroLog Test
    // NOTE: You MUST run the consumer in a separate terminal for this to finish!
    benchmark_aerolog();

    return 0;
}