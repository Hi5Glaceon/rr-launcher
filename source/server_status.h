#ifndef RR_SERVER_STATUS_H
#define RR_SERVER_STATUS_H

#include <stdbool.h>

bool rr_server_status_fetch(void);

/*
 * Returns whether a server status fetch has already completed
 * successfully and its result is cached (regardless of which caller
 * triggered that fetch). Use this to avoid a redundant re-fetch when
 * something earlier in the boot sequence may have already fetched it.
 */
bool rr_server_status_is_cached(void);

bool rr_server_status_test(void);
bool rr_server_status_has_alert(void);
const char *rr_server_status_alert_color(void);

#endif