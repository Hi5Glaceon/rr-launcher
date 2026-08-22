#include "stun.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>

#define STUN_MAGIC_COOKIE 0x2112A442u
#define STUN_CHANGE_REQUEST 0x0003u
#define STUN_RESPONSE_ORIGIN 0x802Bu
#define STUN_OTHER_ADDRESS   0x802Cu
#define STUN_XOR_MAPPED_ADDRESS 0x0020u
#define STUN_MAPPED_ADDRESS     0x0001u

static uint32_t rr_stun_transaction_counter = 1;

static void rr_stun_make_transaction_id(uint8_t *id)
{
    uint32_t counter = rr_stun_transaction_counter++;

    id[0] = 'R';
    id[1] = 'R';
    id[2] = 'N';
    id[3] = 'T';
    id[4] = (uint8_t)(counter >> 24);
    id[5] = (uint8_t)(counter >> 16);
    id[6] = (uint8_t)(counter >> 8);
    id[7] = (uint8_t)counter;
    id[8] = 0x52;
    id[9] = 0x52;
    id[10] = 0x53;
    id[11] = 0x01;
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
for (;;) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);

        struct timeval wait_time;
        wait_time.tv_sec = timeout_ms / 1000;
        wait_time.tv_usec = (timeout_ms % 1000) * 1000;

        int ready = select(sock + 1, &readfds, NULL, NULL, &wait_time);

        if (ready <= 0)
            return false;

        uint8_t buf[1024];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);

        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from, &from_len);

        if (n < 20)
            continue;

        if (buf[0] != 0x01 || buf[1] != 0x01)
            continue;

        uint16_t msg_len =
            (uint16_t)((buf[2] << 8) | buf[3]);

        if ((uint32_t)msg_len + 20u > (uint32_t)n)
            continue;

        if (memcmp(buf + 8, transaction_id, 12) != 0)
            continue;

        if (response_from)
            *response_from = from;

        if (got_response)
            *got_response = true;

        return true;
    }
}

static bool rr_stun_binding_internal(int sock, const char *server,
                                     uint16_t server_port,
                                     uint32_t timeout_ms,
                                     rr_stun_result_t *result,
                                     uint32_t *response_origin_ip,
                                     uint16_t *response_origin_port,
                                     uint32_t *other_ip,
                                     uint16_t *other_port)
{
    if (sock < 0 || !server || !result)
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

    struct sockaddr_in dst;

    if (!rr_stun_resolve(server, server_port, &dst))
        return false;

    uint8_t transaction_id[12];
    rr_stun_make_transaction_id(transaction_id);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               &tv, sizeof(tv));

    uint8_t req[20] = {0};

    req[1] = 0x01;

    req[4] = 0x21;
    req[5] = 0x12;
    req[6] = 0xA4;
    req[7] = 0x42;

    memcpy(req + 8, transaction_id, 12);

    if (sendto(sock, req, sizeof(req), 0,
               (const struct sockaddr *)&dst,
               sizeof(dst)) < 0)
        return false;

    uint8_t buf[1024];

    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);

    int n = recvfrom(sock, buf, sizeof(buf), 0,
                     (struct sockaddr *)&from, &from_len);

    if (n < 20 || buf[0] != 0x01 || buf[1] != 0x01)
        return false;

    uint16_t msg_len =
        (uint16_t)((buf[2] << 8) | buf[3]);

    if ((uint32_t)msg_len + 20u > (uint32_t)n)
        return false;

    if (memcmp(buf + 8, transaction_id, 12) != 0)
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

bool rr_stun_binding_socket(int sock, const char *server,
                            uint16_t server_port,
                            uint32_t timeout_ms,
                            rr_stun_result_t *result)
{
    return rr_stun_binding_internal(
        sock,
        server,
        server_port,
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

    /*
     * Initial RFC 5780 discovery.
     */
    if (!rr_stun_binding_internal(
            sock,
            server,
            server_port,
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

    struct sockaddr_in primary;

    if (!rr_stun_resolve(server, server_port, &primary))
        return false;

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
