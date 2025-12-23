#include <iostream>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

// Unique name for the shared memory segment
#define SHM_NAME "/aerolog_buffer"
#define SHM_SIZE 4096 // Start with 4KB for testing

struct SharedMessage {
    char text[256];
    bool ready; // A simple flag to signal data is written
};