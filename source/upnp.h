#ifndef RR_UPNP_H
#define RR_UPNP_H
#include <stdint.h>
#include <stdbool.h>
#include "upnp_error_codes.h"

typedef struct {
    char location[256];
    char control_url[256];
    char service_type[128];
    char host[64];
    uint16_t port;
    bool valid;
} rr_upnp_device_t;

bool rr_upnp_discover(rr_upnp_device_t *device, uint32_t timeout_ms);
bool rr_upnp_add_udp_mapping(const rr_upnp_device_t *device, uint16_t external_port,
                             uint16_t internal_port, const char *description,
                             uint32_t lease_seconds);
bool rr_upnp_delete_udp_mapping(const rr_upnp_device_t *device, uint16_t external_port,
                                uint16_t internal_port);
bool rr_upnp_prepare_mkwii_mapping(uint16_t *port_out);

/* Most recent UPnP diagnostic. Code 0 means no router SOAP error code. */
const rr_upnp_error_info_t *rr_upnp_last_error(void);
#endif
