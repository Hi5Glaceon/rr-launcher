#ifndef RR_SERVER_STATUS_H
#define RR_SERVER_STATUS_H

#include <stdbool.h>

bool rr_server_status_fetch(void);
bool rr_server_status_test(void);
bool rr_server_status_has_alert(void);
const char *rr_server_status_alert_color(void);

#endif