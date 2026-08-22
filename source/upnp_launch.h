#ifndef RR_UPNP_H
#define RR_UPNP_H
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char location[256];
    char control_url[256];
    char service_type[128];
    bool valid;
} rr_upnp_device_t;

bool rr_upnp_discover(rr_upnp_device_t *device, uint32_t timeout_ms);
bool rr_upnp_get_external_ip(const rr_upnp_device_t *device,
                             char *ip, uint32_t ip_size);
bool rr_upnp_add_udp_mapping(const rr_upnp_device_t *device,
                             uint16_t external_port,
                             uint16_t internal_port,
                             const char *description,
                             uint32_t lease_seconds);
bool rr_upnp_delete_udp_mapping(const rr_upnp_device_t *device,
                                uint16_t external_port,
                                uint16_t internal_port);

bool rr_upnp_prepare_mkwii_mapping(uint16_t *port_out);
#endif
