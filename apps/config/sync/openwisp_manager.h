#ifndef OPENWISP_MANAGER_H
#define OPENWISP_MANAGER_H

#include <stdbool.h>

/**
 * Configure OpenWisp to ignore fry-managed sections
 * Returns 0 on success, -1 on error
 */
int configure_openwisp_exclusions(bool dev_mode);

#endif /* OPENWISP_MANAGER_H */