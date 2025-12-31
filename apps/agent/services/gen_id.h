#ifndef GENID_H
#define GENID_H

#include <time.h>
#include <unistd.h>

char generate_id(char *id, size_t id_size, char *fry_device_id, time_t timestamp);

#endif /* GENID_H */
