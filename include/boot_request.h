#ifndef BOOT_REQUEST_H
#define BOOT_REQUEST_H

#include <stdbool.h>

void boot_request_set(void);
void boot_request_clear(void);
bool boot_request_is_set(void);

#endif
