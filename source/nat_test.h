#ifndef RR_NAT_TEST_H
#define RR_NAT_TEST_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    RR_NAT_UNKNOWN = 0,
    RR_NAT_ENDPOINT_INDEPENDENT,
    RR_NAT_SYMMETRIC
} rr_nat_type_t;

typedef enum {
    RR_NAT_FILTER_UNKNOWN = 0,
    RR_NAT_FILTER_ENDPOINT_INDEPENDENT,
    RR_NAT_FILTER_ADDRESS_DEPENDENT,
    RR_NAT_FILTER_ADDRESS_PORT_DEPENDENT
} rr_nat_filter_type_t;

typedef enum {
    RR_P2P_COMPATIBILITY_UNKNOWN = 0,
    RR_P2P_COMPATIBILITY_GOOD,
    RR_P2P_COMPATIBILITY_FAIR,
    RR_P2P_COMPATIBILITY_BAD
} rr_p2p_compatibility_t;

/* Ping test result */
typedef struct {
    bool success;
    uint32_t min_ms;
    uint32_t avg_ms;
    uint32_t max_ms;
} rr_ping_result_t;

/* NAT test result */
typedef struct {
    bool stun_server2;
    bool stun_server1;
    bool stun_behavior;

    uint32_t public_ip_server2;
    uint16_t public_port_server2;

    uint32_t public_ip_server1;
    uint16_t public_port_server1;

    bool udp_outbound;

    rr_nat_type_t nat_type;
    rr_nat_filter_type_t nat_filter_type;

    bool udp_inbound;
    int p2p_compatibility;

} rr_nat_result_t;

bool rr_nat_test_run(rr_nat_result_t *result);
bool rr_ping_test_run(rr_ping_result_t *result);

#endif