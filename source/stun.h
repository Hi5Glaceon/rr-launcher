#ifndef RR_STUN_H
#define RR_STUN_H

#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>


typedef struct {
    bool success;

    uint32_t public_ip;
    uint16_t public_port;

} rr_stun_result_t;


typedef struct {
    bool success;
    bool change_port;
    bool change_ip;
    bool change_ip_port;
} rr_stun_behavior_result_t;


/*
 * Perform a STUN Binding request using a new UDP socket.
 */
bool rr_stun_binding(
    const char *server,
    uint16_t server_port,
    uint32_t timeout_ms,
    rr_stun_result_t *result
);


/* Resolve a STUN server once so multiple requests can reuse the result. */
bool rr_stun_resolve_server(
    const char *server,
    uint16_t server_port,
    struct sockaddr_in *dst
);

/*
 * Perform a STUN Binding request using an existing UDP socket.
 *
 * Multiple calls using the same socket allow us to compare
 * the public mapping for different STUN destinations.
 */
bool rr_stun_binding_socket(
    int sock,
    const char *server,
    uint16_t server_port,
    uint32_t timeout_ms,
    rr_stun_result_t *result
);


/*
 * Perform a STUN Binding request using an existing socket and an
 * already-resolved destination address. This avoids repeating DNS
 * resolution when multiple requests use the same STUN server.
 */
bool rr_stun_binding_socket_addr(
    int sock,
    const struct sockaddr_in *dst,
    uint32_t timeout_ms,
    rr_stun_result_t *result
);


/*
 * Perform RFC 5780 NAT filtering tests against a STUN server
 * that advertises RESPONSE-ORIGIN and OTHER-ADDRESS.
 *
 * The same UDP socket is used for all requests. The server is
 * expected to change its source port/IP according to the
 * CHANGE-REQUEST attribute.
 */
bool rr_stun_behavior_test(
    int sock,
    const char *server,
    uint16_t server_port,
    uint32_t timeout_ms,
    rr_stun_behavior_result_t *result
);


#endif
