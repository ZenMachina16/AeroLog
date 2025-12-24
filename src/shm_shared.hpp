#include <iostream>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic> // Essential for thread-safe counters

#define SHM_NAME "/aerolog_buffer"
#define RING_SIZE 1024 // Power of 2 is best for performance

// Define what a single log entry looks like
struct LogEntry {
    uint64_t timestamp;
    int event_id;
    double value;
};

// The Shared Memory Layout
struct SharedBuffer {
    std::atomic<uint64_t> head; // Managed by Consumer (Sidecar)
    std::atomic<uint64_t> tail; // Managed by Producer (App)
    LogEntry entries[RING_SIZE];
};