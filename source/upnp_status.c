#include "upnp_status.h"
#include "private_port.h"
#include "upnp.h"

rr_upnp_status_t rr_upnp_status_run(uint16_t *port_out)
{
    if (port_out) *port_out = 0;
    uint16_t port;
    if (!rr_get_mkwii_private_port(&port))
        return RR_UPNP_STATUS_FAILED;
    if (!rr_upnp_prepare_mkwii_mapping(&port))
        return RR_UPNP_STATUS_FAILED;
    if (port_out) *port_out = port;
    return RR_UPNP_STATUS_SUCCESS;
}

const char *rr_upnp_status_string(rr_upnp_status_t status)
{
    switch (status) {
        case RR_UPNP_STATUS_PENDING: return "PENDING";
        case RR_UPNP_STATUS_SUCCESS: return "SUCCESS";
        default: return "FAILED";
    }
}
