#ifndef RR_PRIVATE_PORT_H
#define RR_PRIVATE_PORT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

bool rr_get_mkwii_private_port(uint16_t *port);

bool rr_get_mkwii_private_port_debug(uint16_t *port,
                                     char *status,
                                     size_t status_size);

#endif