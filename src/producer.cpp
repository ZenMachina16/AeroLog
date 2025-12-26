#include "shm_shared.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cstring> // for strerror

int main() {
    // 1. Setup Shared Memory
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd == -1) { perror("shm_open failed"); return 1; }

    if (ftruncate(fd, sizeof(SharedBuffer)) == -1) { perror("ftruncate failed"); return 1; }

    SharedBuffer* ring = (SharedBuffer*)mmap(0, sizeof(SharedBuffer), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ring == MAP_FAILED) { perror("mmap failed"); return 1; }

    // Initialize pointers to 0 (clean slate)
    ring->head.store(0);
    ring->tail.store(0);

    // 2. Open the CSV File
    // Note: ensure trades.csv is in the folder ABOVE src, or change path to "./trades.csv" if it's in src
    std::string csv_path = "../trades.csv"; 
    std::ifstream file(csv_path);

    if (!file.is_open()) {
        std::cerr << "Error: Could not open " << csv_path << std::endl;
        std::cerr << "Make sure the file exists and is in the project root folder." << std::endl;
        return 1;
    }

    std::string line;
    // Skip the header row (Timestamp, Open, High...)
    std::getline(file, line); 

    std::cout << "Producer: Replaying market data from CSV..." << std::endl;
    int count = 0;

    // 3. Read and Send Loop
    while (std::getline(file, line)) {
        std::stringstream ss(line);
        std::string segment;
        std::vector<std::string> row;

        // Split line by comma
        while (std::getline(ss, segment, ',')) {
            row.push_back(segment);
        }

        // Safety: Ensure row has enough columns (Timestamp is col 0, Close is col 4)
        if (row.size() < 5) continue; 

        try {
            // FIX 1: Parse Timestamp (handle ".0" if present)
            uint64_t timestamp = (uint64_t)std::stod(row[0]);
            
            // FIX 2: Parse Price (Index 4 = "Close" price)
            double price = std::stod(row[4]); 

            // --- CRITICAL SECTION: Write to Ring Buffer ---
            uint64_t current_tail = ring->tail.load(std::memory_order_relaxed);
            uint64_t next_tail = (current_tail + 1) % RING_SIZE;

            // Spin-wait if buffer is full
            while (next_tail == ring->head.load(std::memory_order_acquire)) {
                // In a real low-latency app, we might just drop the packet here.
                // For this demo, we wait to ensure data integrity.
            }

            LogEntry& entry = ring->entries[current_tail];
            entry.event_id = count;       
            entry.timestamp = timestamp;  
            entry.value = price;          

            // Commit the write
            ring->tail.store(next_tail, std::memory_order_release);
            // ----------------------------------------------
            
            count++;
        } catch (...) {
            continue; // Skip lines that fail to parse
        }
    }

    std::cout << "Producer finished. Replayed " << count << " trades." << std::endl;
    return 0;
}