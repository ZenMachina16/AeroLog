#include "shm_shared.hpp"

int main() {
    // 1. Open the existing shared memory
    int fd = shm_open(SHM_NAME, O_RDONLY, 0666);

    // 2. Map it
    SharedMessage* msg = (SharedMessage*)mmap(0, SHM_SIZE, PROT_READ, MAP_SHARED, fd, 0);

    // 3. Wait for the flag and read
    while (!msg->ready) { 
        usleep(1000); // Wait 1ms
    }

    std::cout << "Sidecar received: " << msg->text << std::endl;

    // 4. Cleanup
    shm_unlink(SHM_NAME); 
    return 0;
}