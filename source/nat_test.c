#include "nat_test.h"
#include "stun.h"
#include "private_port.h"

#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>

#define STUN_SERVER1 "stun.t-online.de"
#define STUN_SERVER1_PORT  3478
#define STUN_SERVER2   "stun.cloudflare.com"
#define STUN_SERVER2_PORT     3478
#define STUN_BEHAVIOR_SERVER "stunserver2025.stunprotocol.org"
#define STUN_BEHAVIOR_PORT   3478
#define STUN_ANYCAST_SERVER "stun-anycast.l.google.com"
#define STUN_ANYCAST_PORT   19302
#define STUN_PING_COUNT 5
#define STUN_PING_TIMEOUT_MS 2000

static int rr_open_mkwii_udp_socket(void)
{
    uint16_t port;
    if (!rr_get_mkwii_private_port(&port))
        return -1;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(port);

    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

static uint32_t rr_ping_elapsed_ms(const struct timeval *start, const struct timeval *end)
{
    int64_t sec = (int64_t)end->tv_sec - (int64_t)start->tv_sec;
    int64_t usec = (int64_t)end->tv_usec - (int64_t)start->tv_usec;
    if (usec < 0) { --sec; usec += 1000000; }
    return (uint32_t)(sec * 1000 + usec / 1000);
}

static int rr_ping_socket = -1;

static int rr_get_ping_socket(void)
{
    if (rr_ping_socket >= 0)
        return rr_ping_socket;

    rr_ping_socket = rr_open_mkwii_udp_socket();
    return rr_ping_socket;
}

static void rr_close_ping_socket(void)
{
    if (rr_ping_socket >= 0) {
        close(rr_ping_socket);
        rr_ping_socket = -1;
    }
}

bool rr_ping_test_run(rr_ping_result_t *result)
{
    if (!result) return false;
    memset(result, 0, sizeof(*result));

    int sock = rr_get_ping_socket();
    if (sock < 0) return false;

    /*
     * Resolve the ping target once and reuse the address for all
     * STUN_PING_COUNT rounds below, instead of re-resolving via DNS on
     * every single ping. This avoids paying DNS lookup time on every
     * round (which previously leaked into the measured RTT) and avoids
     * a transient DNS hiccup on one round counting as a "lost" ping
     * even though the UDP path itself was fine.
     */
    struct sockaddr_in dst;
    if (!rr_stun_resolve_server(STUN_ANYCAST_SERVER, STUN_ANYCAST_PORT, &dst))
        return false;

    uint32_t total_ms = 0, successful = 0;
    result->min_ms = 0xffffffff;

    for (int i = 0; i < STUN_PING_COUNT; ++i) {
        struct timeval start, end;
        gettimeofday(&start, NULL);
        bool ok = rr_stun_ping_socket_addr(sock, &dst, STUN_PING_TIMEOUT_MS);
        gettimeofday(&end, NULL);
        if (!ok) continue;
        uint32_t elapsed = rr_ping_elapsed_ms(&start, &end);
        if (elapsed < result->min_ms) result->min_ms = elapsed;
        if (elapsed > result->max_ms) result->max_ms = elapsed;
        total_ms += elapsed;
        ++successful;
    }

    if (!successful) {
        /*
         * If the socket is completely unusable, discard it so the next
         * test can recover by creating a fresh socket.
         */
        rr_close_ping_socket();

        result->success = false;
        result->min_ms = result->avg_ms = result->max_ms = 0;
        return false;
    }

    result->success = true;
    result->avg_ms = total_ms / successful;
    return true;
}

bool rr_nat_test_run(rr_nat_result_t *result)
{
    if (!result) return false;
    memset(result, 0, sizeof(*result));

    rr_close_ping_socket();

    uint16_t private_port;
    if (!rr_get_mkwii_private_port(&private_port))
        return false;

    rr_stun_result_t server1, server2;
    rr_stun_behavior_result_t behavior;
    memset(&server1, 0, sizeof(server1));
    memset(&server2, 0, sizeof(server2));
    memset(&behavior, 0, sizeof(behavior));

    int sock = rr_open_mkwii_udp_socket();
    if (sock < 0) return false;

    /*
     * Resolve each STUN server once and reuse the same address for the
     * initial attempt and the retry below. Re-resolving per attempt
     * meant a retry could silently land on a different address than the
     * first attempt for hosts with multiple DNS records, which made the
     * "retry" test a different server rather than a second try against
     * the same one.
     */
    struct sockaddr_in server1_addr, server2_addr;
    bool have_server1_addr =
        rr_stun_resolve_server(STUN_SERVER1, STUN_SERVER1_PORT, &server1_addr);
    bool have_server2_addr =
        rr_stun_resolve_server(STUN_SERVER2, STUN_SERVER2_PORT, &server2_addr);

    /*
     * A freshly-created Wii UDP socket can occasionally lose the first
     * STUN transaction. Retry each normal binding once before reporting
     * failure. The existing per-request 10 s timeout is unchanged.
     */
    result->stun_server1 = have_server1_addr &&
        rr_stun_binding_socket_addr(sock, &server1_addr, 10000, &server1);

    if (!result->stun_server1 && have_server1_addr) {
        memset(&server1, 0, sizeof(server1));
        result->stun_server1 =
            rr_stun_binding_socket_addr(sock, &server1_addr, 10000, &server1);
    }

    if (result->stun_server1) {
        result->public_ip_server1 = server1.public_ip;
        result->public_port_server1 = server1.public_port;
        result->udp_outbound = true;
    }

    result->stun_server2 = have_server2_addr &&
        rr_stun_binding_socket_addr(sock, &server2_addr, 10000, &server2);

    if (!result->stun_server2 && have_server2_addr) {
        memset(&server2, 0, sizeof(server2));
        result->stun_server2 =
            rr_stun_binding_socket_addr(sock, &server2_addr, 10000, &server2);
    }

    if (result->stun_server2) {
        result->public_ip_server2 = server2.public_ip;
        result->public_port_server2 = server2.public_port;
        result->udp_outbound = true;
    }

    if (result->stun_server1 && result->stun_server2) {
        result->nat_type =
            (server1.public_ip == server2.public_ip &&
             server1.public_port == server2.public_port)
            ? RR_NAT_ENDPOINT_INDEPENDENT : RR_NAT_SYMMETRIC;
    } else {
        result->nat_type = RR_NAT_UNKNOWN;
    }

    close(sock);

    int behavior_sock = rr_open_mkwii_udp_socket();
    if (behavior_sock >= 0) {
        if (rr_stun_behavior_test(behavior_sock, STUN_BEHAVIOR_SERVER,
                                  STUN_BEHAVIOR_PORT, 10000, &behavior)) {
            result->stun_behavior = true;
            if (behavior.change_ip_port)
                result->nat_filter_type = RR_NAT_FILTER_ENDPOINT_INDEPENDENT;
            else if (behavior.change_port)
                result->nat_filter_type = RR_NAT_FILTER_ADDRESS_DEPENDENT;
            else
                result->nat_filter_type = RR_NAT_FILTER_ADDRESS_PORT_DEPENDENT;
        }
        close(behavior_sock);
    }

    result->udp_inbound = result->stun_behavior;
    result->p2p_compatibility = RR_P2P_COMPATIBILITY_UNKNOWN;

    if (result->nat_type == RR_NAT_SYMMETRIC) {
        result->p2p_compatibility = RR_P2P_COMPATIBILITY_BAD;
    } else if (result->nat_type == RR_NAT_ENDPOINT_INDEPENDENT) {
        if (result->nat_filter_type == RR_NAT_FILTER_ENDPOINT_INDEPENDENT ||
            result->nat_filter_type == RR_NAT_FILTER_ADDRESS_DEPENDENT)
            result->p2p_compatibility = RR_P2P_COMPATIBILITY_GOOD;
        else if (result->nat_filter_type == RR_NAT_FILTER_ADDRESS_PORT_DEPENDENT)
            result->p2p_compatibility = RR_P2P_COMPATIBILITY_FAIR;
    }

    (void)private_port;

    /*
     * Reaching the end of the NAT test means the test itself completed.
     * Individual STUN/behavior results are reported through rr_nat_result_t
     * and must not make the whole test appear "not completed".
     */
    return true;
}
