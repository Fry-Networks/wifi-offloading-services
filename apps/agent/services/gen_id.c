#include <stdio.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

void generate_id(char *id, size_t id_size, char *fry_device_id, time_t timestamp) {
    pid_t process_id = getpid();
    snprintf(id, id_size, "%s_%ld_%d", fry_device_id, (long)timestamp, process_id);
}