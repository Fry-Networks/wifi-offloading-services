#ifndef REGISTRATION_H
#define REGISTRATION_H

#include <stdbool.h>

typedef struct {
    char *fry_device_id;
    char *access_key;
} Registration;

Registration *init_registration(char *mac, char *model, char *brand, char *openwisp_device_id);
void clean_registration(Registration *registration);

#endif /* REGISTRATION_H */
