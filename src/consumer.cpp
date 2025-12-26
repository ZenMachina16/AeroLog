#include "shm_shared.hpp"
#include <cstdio>
#include <vector>

// Configuration
const int BATCH_SIZE = 1000; // Write to disk only after collecting 1000 logs

int main() {
    int fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (fd == -1) { perror("shm_open failed"); return 1; }

    SharedBuffer* ring = (SharedBuffer*)mmap(0, sizeof(SharedBuffer), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ring == MAP_FAILED) { perror("mmap failed"); return 1; }

    FILE* fp = fopen("telemetry.alog", "wb");
    if (!fp) { perror("fopen failed"); return 1; }

    // Local buffer to hold data before flushing to disk
    // This reduces disk I/O calls by factor of 1000
    std::vector<LogEntry> disk_buffer;
    disk_buffer.reserve(BATCH_SIZE);

    uint64_t processed_count = 0;
    std::cout << "Sidecar: Optimized Batching Mode Active..." << std::endl;

    while (processed_count < 1000000) { // Matching the 1M logs from benchmark
        uint64_t current_head = ring->head.load(std::memory_order_relaxed);
        
        if (current_head != ring->tail.load(std::memory_order_acquire)) {
            // 1. Read from Shared Memory
            LogEntry& entry = ring->entries[current_head];
            
            // 2. Add to local RAM buffer (Super fast)
            disk_buffer.push_back(entry);

            // 3. If buffer is full, FLUSH to disk (The Optimization)
            if (disk_buffer.size() >= BATCH_SIZE) {
                fwrite(disk_buffer.data(), sizeof(LogEntry), disk_buffer.size(), fp);
                disk_buffer.clear();
            }

            processed_count++;
            ring->head.store((current_head + 1) % RING_SIZE, std::memory_order_release);
        } else {
             // Optional: If idle, reduce CPU usage
            // std::this_thread::yield(); 
        }
    }

    // Flush any remaining logs
    if (!disk_buffer.empty()) {
        fwrite(disk_buffer.data(), sizeof(LogEntry), disk_buffer.size(), fp);
    }

    std::cout << "Sidecar: Finished. Total processed: " << processed_count << std::endl;
    
    fclose(fp);
    shm_unlink(SHM_NAME);
    return 0;
}