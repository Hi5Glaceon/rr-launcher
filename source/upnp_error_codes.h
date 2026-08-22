#ifndef RR_UPNP_ERROR_CODES_H
#define RR_UPNP_ERROR_CODES_H

#include <stdbool.h>

typedef struct
{
    int code;
    bool known;
    char name[64];
    char description[256];
    char solution[256];
} rr_upnp_error_info_t;

bool rr_upnp_error_lookup(int code, rr_upnp_error_info_t *result);

#endif
