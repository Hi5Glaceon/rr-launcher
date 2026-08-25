#include "stun.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>


static uint32_t rr_stun_remaining_ms(const struct timeval *start,
                                     uint32_t timeout_ms)
{
    struct timeval now;
    gettimeofday(&now, NULL);

    int64_t elapsed_us =
        ((int64_t)now.tv_sec - (int64_t)start->tv_sec) * 1000000LL +
        ((int64_t)now.tv_usec - (int64_t)start->tv_usec);

    int64_t remaining_us =
        (int64_t)timeout_ms * 1000LL - elapsed_us;

    if (remaining_us <= 0)
        return 0;

    return (uint32_t)((remaining_us + 999LL) / 1000LL);
}

static int rr_stun_recv_until(int sock, uint8_t *buf, size_t buf_size,
                              struct sockaddr_in *from, socklen_t *from_len,
                              const struct timeval *start,
                              uint32_t timeout_ms)
{
    for (;;) {
        uint32_t remaining_ms =
            rr_stun_remaining_ms(start, timeout_ms);

        if (remaining_ms == 0)
            return -2;

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);

        struct timeval wait_time;
        wait_time.tv_sec = remaining_ms / 1000;
        wait_time.tv_usec = (remaining_ms % 1000) * 1000;

        int ready = select(sock + 1, &readfds, NULL, NULL, &wait_time);

        if (ready < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (ready == 0)
            return -2;

        *from_len = sizeof(*from);
        int n = recvfrom(sock, buf, buf_size, 0,
                         (struct sockaddr *)from, from_len);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(10000);
                continue;
            }
            return -1;
        }

        return n;
    }
}

/*
 * Waits (within the remaining time budget defined by `start`/`timeout_ms`)
 * for a STUN Binding Success Response whose transaction ID matches
 * `transaction_id`. Any packet that is too short, has the wrong STUN
 * message type, has an inconsistent message-length field, or has a
 * mismatching transaction ID is discarded and the wait continues - it
 * does NOT give up after the first packet, since a stray/late/unrelated
 * packet arriving first must not cause the real response to be missed.
 *
 * Returns the response length (>= 20) on success, or a negative value
 * from rr_stun_recv_until() (-1 = socket error, -2 = timeout) otherwise.
 */
static int rr_stun_wait_for_matching_response(
    int sock, uint8_t *buf, size_t buf_size,
    const uint8_t transaction_id[12],
    struct sockaddr_in *from,
    const struct timeval *start, uint32_t timeout_ms)
{
    for (;;) {
        socklen_t from_len = sizeof(*from);

        int n = rr_stun_recv_until(sock, buf, buf_size, from, &from_len,
                                    start, timeout_ms);

        if (n < 0)
            return n;

        if (n < 20)
            continue;

        if (buf[0] != 0x01 || buf[1] != 0x01)
            continue;

        uint16_t msg_len = (uint16_t)((buf[2] << 8) | buf[3]);

        if ((uint32_t)msg_len + 20u > (uint32_t)n)
            continue;

        if (memcmp(buf + 8, transaction_id, 12) != 0)
            continue;

        return n;
    }
}


#define STUN_MAGIC_COOKIE 0x2112A442u
#define STUN_CHANGE_REQUEST 0x0003u
#define STUN_RESPONSE_ORIGIN 0x802Bu
#define STUN_OTHER_ADDRESS   0x802Cu
#define STUN_XOR_MAPPED_ADDRESS 0x0020u
#define STUN_MAPPED_ADDRESS     0x0001u

static bool rr_stun_rng_seeded = false;

/*
 * RFC 5389 requires the 96-bit transaction ID to be chosen so it is
 * "uniformly and randomly distributed" - among other things this
 * makes it hard for an off-path attacker to guess/spoof a response.
 * The previous version used 8 fixed bytes ('R','R','N','T', ...) plus
 * a simple incrementing counter, which is trivially predictable.
 * rand() isn't cryptographically secure either, but it's a large
 * improvement over a near-constant value and is what's available
 * here without pulling in extra dependencies.
 */
static void rr_stun_make_transaction_id(uint8_t *id)
{
    if (!rr_stun_rng_seeded)
    {
        struct timeval seed_tv;
        gettimeofday(&seed_tv, NULL);
        srand((unsigned)(seed_tv.tv_sec ^ seed_tv.tv_usec));
        rr_stun_rng_seeded = true;
    }

    for (int i = 0; i < 12; ++i)
        id[i] = (uint8_t)(rand() & 0xff);
}

static bool rr_stun_resolve(const char *server, uint16_t server_port,
                            struct sockaddr_in *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->sin_family = AF_INET;
    dst->sin_port = htons(server_port);

    if (inet_aton(server, &dst->sin_addr) != 0)
        return true;

    struct hostent *he = gethostbyname(server);
    if (!he)
        return false;

    memcpy(&dst->sin_addr, he->h_addr, sizeof(dst->sin_addr));
    return true;
}

/*
 * Public wrapper so callers that issue multiple requests to the same
 * server (retries, repeated pings, ...) can resolve once and reuse the
 * result, instead of re-resolving (and potentially getting a different
 * address, or paying the DNS latency again) on every single request.
 */
bool rr_stun_resolve_server(const char *server, uint16_t server_port,
                            struct sockaddr_in *dst)
{
    if (!server || !dst)
        return false;

    return rr_stun_resolve(server, server_port, dst);
}

static bool rr_stun_parse_address(const uint8_t *value, uint16_t len,
                                  uint32_t *ip, uint16_t *port,
                                  bool xor_address,
                                  const uint8_t *transaction_id)
{
    if (len < 8 || value[1] != 0x01 || !ip || !port)
        return false;

    uint16_t p = (uint16_t)((value[2] << 8) | value[3]);
    uint32_t addr;

    memcpy(&addr, value + 4, 4);

    if (xor_address) {
        p ^= (uint16_t)(STUN_MAGIC_COOKIE >> 16);
        addr ^= htonl(STUN_MAGIC_COOKIE);
        (void)transaction_id;
    }

    *port = p;
    *ip = ntohl(addr);

    return true;
}

/*
 * Send a STUN Binding request, optionally containing CHANGE-REQUEST.
 *
 * change_flags:
 *
 *   0x02 = change port
 *   0x04 = change IP
 *   0x06 = change IP + port
 */
static bool rr_stun_send_request(int sock, const struct sockaddr_in *dst,
                                  uint32_t timeout_ms, uint32_t change_flags,
                                  uint8_t transaction_id[12],
                                  struct sockaddr_in *response_from,
                                  bool *got_response)
{
    uint8_t req[28] = {0};
    uint16_t attr_len = change_flags ? 8 : 0;

    req[0] = 0x00;
    req[1] = 0x01;
    req[2] = (uint8_t)(attr_len >> 8);
    req[3] = (uint8_t)attr_len;
    req[4] = 0x21;
    req[5] = 0x12;
    req[6] = 0xA4;
    req[7] = 0x42;

    memcpy(req + 8, transaction_id, 12);

    if (change_flags) {
        req[20] = 0x00;
        req[21] = 0x03;
        req[22] = 0x00;
        req[23] = 0x04;
        req[24] = (uint8_t)(change_flags >> 24);
        req[25] = (uint8_t)(change_flags >> 16);
        req[26] = (uint8_t)(change_flags >> 8);
        req[27] = (uint8_t)change_flags;
    }

    if (got_response)
        *got_response = false;

    if (sendto(sock, req, 20 + attr_len, 0,
               (const struct sockaddr *)dst, sizeof(*dst)) < 0)
        return false;

    struct timeval start;
    gettimeofday(&start, NULL);

    uint8_t buf[1024];
    struct sockaddr_in from;

    int n = rr_stun_wait_for_matching_response(
        sock, buf, sizeof(buf), transaction_id, &from, &start, timeout_ms);

    if (n < 0)
        return false;

    if (response_from)
        *response_from = from;

    if (got_response)
        *got_response = true;

    return true;
}

static bool rr_stun_binding_internal_addr(int sock,
                                     const struct sockaddr_in *dst,
                                     uint32_t timeout_ms,
                                     rr_stun_result_t *result,
                                     uint32_t *response_origin_ip,
                                     uint16_t *response_origin_port,
                                     uint32_t *other_ip,
                                     uint16_t *other_port)
{
    if (sock < 0 || !dst || !result)
        return false;

    memset(result, 0, sizeof(*result));

    if (response_origin_ip)
        *response_origin_ip = 0;

    if (response_origin_port)
        *response_origin_port = 0;

    if (other_ip)
        *other_ip = 0;

    if (other_port)
        *other_port = 0;

    uint8_t transaction_id[12];
    rr_stun_make_transaction_id(transaction_id);

    uint8_t req[20] = {0};

    req[1] = 0x01;

    req[4] = 0x21;
    req[5] = 0x12;
    req[6] = 0xA4;
    req[7] = 0x42;

    memcpy(req + 8, transaction_id, 12);

    if (sendto(sock, req, sizeof(req), 0,
               (const struct sockaddr *)dst,
               sizeof(*dst)) < 0)
        return false;

    struct timeval start;
    gettimeofday(&start, NULL);

    uint8_t buf[1024];
    struct sockaddr_in from;

    /*
     * Wait for the response that actually matches this request's
     * transaction ID, discarding anything else (short/garbled packets,
     * stray responses left over from a previous request on this reused
     * socket, etc.) until either a match arrives or the timeout expires.
     * Previously this only inspected a single received packet and gave
     * up immediately on any mismatch, which could report FAIL even
     * though the real response arrived a moment later (confirmed via
     * Wireshark).
     */
    int n = rr_stun_wait_for_matching_response(
        sock, buf, sizeof(buf), transaction_id, &from, &start, timeout_ms);

    if (n < 0)
        return false;

    size_t off = 20;

    while (off + 4 <= (size_t)n) {
        uint16_t type =
            (uint16_t)((buf[off] << 8) | buf[off + 1]);

        uint16_t len =
            (uint16_t)((buf[off + 2] << 8) | buf[off + 3]);

        off += 4;

        if (off + len > (size_t)n)
            break;

        if ((type == STUN_XOR_MAPPED_ADDRESS ||
             type == STUN_MAPPED_ADDRESS) &&
            len >= 8 &&
            buf[off + 1] == 0x01) {

            uint32_t ip;
            uint16_t port;

            if (rr_stun_parse_address(
                    buf + off,
                    len,
                    &ip,
                    &port,
                    type == STUN_XOR_MAPPED_ADDRESS,
                    transaction_id)) {

                result->public_ip = ip;
                result->public_port = port;
            }

        } else if (type == STUN_RESPONSE_ORIGIN &&
                   len >= 8 &&
                   buf[off + 1] == 0x01) {

            rr_stun_parse_address(
                buf + off,
                len,
                response_origin_ip,
                response_origin_port,
                false,
                transaction_id);

        } else if (type == STUN_OTHER_ADDRESS &&
                   len >= 8 &&
                   buf[off + 1] == 0x01) {

            rr_stun_parse_address(
                buf + off,
                len,
                other_ip,
                other_port,
                false,
                transaction_id);
        }

        off += (len + 3u) & ~3u;
    }

    result->success =
        (result->public_ip != 0 ||
         result->public_port != 0);

    (void)from;

    return result->success;
}

bool rr_stun_ping_socket_addr(int sock, const struct sockaddr_in *dst,
                              uint32_t timeout_ms)
{
    if (sock < 0 || !dst)
        return false;

    uint8_t transaction_id[12];
    rr_stun_make_transaction_id(transaction_id);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t req[20] = {0};
    req[1] = 0x01;
    req[4] = 0x21;
    req[5] = 0x12;
    req[6] = 0xA4;
    req[7] = 0x42;
    memcpy(req + 8, transaction_id, 12);

    if (sendto(sock, req, sizeof(req), 0,
               (const struct sockaddr *)dst,
               sizeof(*dst)) < 0)
        return false;

    struct timeval start;
    gettimeofday(&start, NULL);

    uint8_t buf[1024];
    struct sockaddr_in from;

    int n = rr_stun_wait_for_matching_response(
        sock, buf, sizeof(buf), transaction_id, &from, &start, timeout_ms);

    return n >= 0;
}

bool rr_stun_ping_socket(int sock, const char *server,
                         uint16_t server_port, uint32_t timeout_ms)
{
    if (sock < 0 || !server)
        return false;

    struct sockaddr_in dst;
    if (!rr_stun_resolve(server, server_port, &dst))
        return false;

    return rr_stun_ping_socket_addr(sock, &dst, timeout_ms);
}



bool rr_stun_binding_socket(int sock, const char *server,
                            uint16_t server_port,
                            uint32_t timeout_ms,
                            rr_stun_result_t *result)
{
    struct sockaddr_in dst;

    if (!rr_stun_resolve(server, server_port, &dst))
        return false;

    return rr_stun_binding_internal_addr(
        sock,
        &dst,
        timeout_ms,
        result,
        0,
        0,
        0,
        0);
}

bool rr_stun_binding_socket_addr(int sock, const struct sockaddr_in *dst,
                                 uint32_t timeout_ms,
                                 rr_stun_result_t *result)
{
    return rr_stun_binding_internal_addr(
        sock,
        dst,
        timeout_ms,
        result,
        0,
        0,
        0,
        0);
}

bool rr_stun_binding(const char *server, uint16_t server_port,
                     uint32_t timeout_ms,
                     rr_stun_result_t *result)
{
    if (!server || !result)
        return false;

    int s = socket(AF_INET, SOCK_DGRAM, 0);

    if (s < 0)
        return false;

    bool success =
        rr_stun_binding_socket(
            s,
            server,
            server_port,
            timeout_ms,
            result);

    close(s);

    return success;
}

bool rr_stun_behavior_test(int sock, const char *server,
                            uint16_t server_port,
                            uint32_t timeout_ms,
                            rr_stun_behavior_result_t *result)
{
    if (sock < 0 || !server || !result)
        return false;

    memset(result, 0, sizeof(*result));

    uint32_t origin_ip = 0;
    uint32_t other_ip = 0;
    uint16_t origin_port = 0;
    uint16_t other_port = 0;

    rr_stun_result_t binding;
    memset(&binding, 0, sizeof(binding));

    struct sockaddr_in primary;

    if (!rr_stun_resolve(server, server_port, &primary))
        return false;

    /*
     * Initial RFC 5780 discovery. Resolved once above and reused for
     * every request below instead of re-resolving per request.
     */
    if (!rr_stun_binding_internal_addr(
            sock,
            &primary,
            timeout_ms,
            &binding,
            &origin_ip,
            &origin_port,
            &other_ip,
            &other_port)) {
        return false;
    }

    if (!origin_ip || !origin_port ||
        !other_ip || !other_port) {
        return false;
    }

    uint8_t transaction_id[12];
    struct sockaddr_in from;

    memset(&from, 0, sizeof(from));
    rr_stun_make_transaction_id(transaction_id);

    bool got_change_ip_port =
        rr_stun_send_request(
            sock,
            &primary,
            timeout_ms,
            0x06,
            transaction_id,
            &from,
            NULL);

    if (got_change_ip_port) {
        result->change_ip_port = true;
        result->change_port = false;
        result->change_ip = false;
        result->success = true;
        return true;
    }

    memset(&from, 0, sizeof(from));
    rr_stun_make_transaction_id(transaction_id);

    bool got_change_port =
        rr_stun_send_request(
            sock,
            &primary,
            timeout_ms,
            0x02,
            transaction_id,
            &from,
            NULL);

    if (got_change_port) {
        result->change_port = true;
        result->change_ip_port = false;
        result->change_ip = false;
    }

    result->success = true;
    return true;
}
