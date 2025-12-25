#include "shm_shared.hpp"
#include <cstdio> // For FILE, fwrite

int main() {
    // 1. Connect to shared memory
    int fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open failed (Did you run producer first?)");
        return 1;
    }

    SharedBuffer* ring = (SharedBuffer*)mmap(0, sizeof(SharedBuffer), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ring == MAP_FAILED) { perror("mmap failed"); return 1; }

    // 2. Open the Binary Log File
    FILE* fp = fopen("telemetry.alog", "wb"); // 'wb' = Write Binary
    if (!fp) { perror("fopen failed"); return 1; }

    uint64_t processed_count = 0;
    std::cout << "Sidecar: consuming data and writing to disk..." << std::endl;

    while (processed_count < 10000) {
        uint64_t current_head = ring->head.load(std::memory_order_relaxed);
        
        if (current_head != ring->tail.load(std::memory_order_acquire)) {
            LogEntry& entry = ring->entries[current_head];
            
            // 3. WRITE TO DISK (The Magic Line)
            // We write the raw bytes of the struct directly to the file
            fwrite(&entry, sizeof(LogEntry), 1, fp);

            processed_count++;
            ring->head.store((current_head + 1) % RING_SIZE, std::memory_order_release);
        }
    }

    std::cout << "Sidecar: Saved " << processed_count << " logs to telemetry.alog" << std::endl;
    
    fclose(fp); // Close file
    shm_unlink(SHM_NAME); // Remove memory
    return 0;
}