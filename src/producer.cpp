// producer.cpp
#include "shm_shared.hpp"

int main() {
    // 1. Create the shared memory object
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    ftruncate(fd, SHM_SIZE);

    // 2. Map the object into memory
    SharedMessage* msg = (SharedMessage*)mmap(0, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    // 3. Write data
    strcpy(msg->text, "Telemetry Data Point #1");
    msg->ready = true;

    std::cout << "Data written to shared memory." << std::endl;
    return 0;
}
































































































































 