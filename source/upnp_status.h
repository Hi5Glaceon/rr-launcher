#ifndef RR_UPNP_STATUS_H
#define RR_UPNP_STATUS_H
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    RR_UPNP_STATUS_FAILED = 0,
    RR_UPNP_STATUS_PENDING,
    RR_UPNP_STATUS_SUCCESS
} rr_upnp_status_t;

rr_upnp_status_t rr_upnp_status_run(uint16_t *port_out);
const char *rr_upnp_status_string(rr_upnp_status_t status);
#endif
