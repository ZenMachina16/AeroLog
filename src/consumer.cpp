#include "shm_shared.hpp"

int main() {
    int fd = shm_open(SHM_NAME, O_RDONLY, 0666);
    SharedBuffer* ring = (SharedBuffer*)mmap(0, sizeof(SharedBuffer), PROT_READ, MAP_SHARED, fd, 0);

    uint64_t processed_count = 0;

    while (processed_count < 10000) {
        uint64_t current_head = ring->head.load(std::memory_order_relaxed);
        
        // 1. Check if there is new data (is Tail ahead of Head?)
        if (current_head != ring->tail.load(std::memory_order_acquire)) {
            
            // 2. Read the data
            LogEntry& entry = ring->entries[current_head];
            // In Phase 3, we will write this to a file. For now, just count it.
            processed_count++;

            // 3. Move the head forward to clear the slot
            ring->head.store((current_head + 1) % RING_SIZE, std::memory_order_release);
        }
    }

    std::cout << "Sidecar successfully consumed " << processed_count << " logs." << std::endl;
    shm_unlink(SHM_NAME);
    return 0;
}