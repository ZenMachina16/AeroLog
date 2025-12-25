#include "shm_shared.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>

int main() {
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open failed");
        return 1;
    }

    if (ftruncate(fd, sizeof(SharedBuffer)) == -1) {
        perror("ftruncate failed");
        return 1;
    }

    SharedBuffer* ring = (SharedBuffer*)mmap(0, sizeof(SharedBuffer), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ring == MAP_FAILED) {
        perror("mmap failed");
        return 1;
    }

    // Initialize shared memory to a clean state
    ring->head.store(0);
    ring->tail.store(0);

    for (int i = 0; i < 10000; ++i) {
        uint64_t current_tail = ring->tail.load(std::memory_order_relaxed);
        uint64_t next_tail = (current_tail + 1) % RING_SIZE;

        // 1. Check if buffer is full (Don't overwrite unread data)
        while (next_tail == ring->head.load(std::memory_order_acquire)) {
            // Buffer is full, wait for consumer to clear space
        }

        // 2. Write the data to the "Tail" slot
        LogEntry& entry = ring->entries[current_tail];
        entry.event_id = i;
        entry.value = i * 1.5;
        entry.timestamp = std::chrono::system_clock::now().time_since_epoch().count();

        // 3. Move the tail forward to "publish" the data
        ring->tail.store(next_tail, std::memory_order_release);
    }

    std::cout << "Producer finished sending 10,000 logs." << std::endl;
    return 0;
}