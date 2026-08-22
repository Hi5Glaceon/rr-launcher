/*
    gpcm_test.c - GPCM TCP connectivity and stability test.

    The test deliberately performs only the minimal GPCM exchange:
    TCP connect to GPCM port 29900, followed by the documented keepalive
    request "\\ka\\\\final\\". It does not perform a GPCM login or challenge.

    DNS is performed once. The resulting address is then used for all
    measurement rounds so DNS resolution time is not included in the TCP
    quality measurements.
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <ogc/lwp_watchdog.h>

#include "gpcm_test.h"

static const char rr_gpcm_keepalive[] = "\\ka\\\\final\\";
static const char rr_gpcm_response_prefix[] = "\\lc\\";

static uint32_t rr_gpcm_ticks_to_ms(u64 ticks)
{
    return (uint32_t)ticks_to_millisecs(ticks);
}

static void rr_gpcm_set_error(rr_gpcm_test_result_t *result, const char *message)
{
    if (!result)
        return;

    snprintf(result->error_message,
             sizeof(result->error_message),
             "%s",
             message ? message : "Unknown error.");
}

static bool rr_gpcm_single_round(const struct sockaddr_in *server,
                                 uint32_t *connect_ms,
                                 uint32_t *response_ms,
                                 char *error,
                                 size_t error_size)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        snprintf(error, error_size, "Could not create TCP socket.");
        return false;
    }

    struct timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    u64 connect_start = gettime();

    if (connect(sock, (const struct sockaddr *)server, sizeof(*server)) < 0)
    {
        snprintf(error, error_size, "TCP connect failed: %s", strerror(errno));
        close(sock);
        return false;
    }

    *connect_ms = rr_gpcm_ticks_to_ms(gettime() - connect_start);

    u64 response_start = gettime();

    if (send(sock,
             rr_gpcm_keepalive,
             sizeof(rr_gpcm_keepalive) - 1,
             0) < 0)
    {
        snprintf(error, error_size, "GPCM send failed: %s", strerror(errno));
        close(sock);
        return false;
    }

    char buffer[256];
    int received = recv(sock, buffer, sizeof(buffer) - 1, 0);

    if (received <= 0)
    {
        if (received == 0)
            snprintf(error, error_size, "GPCM closed the connection.");
        else
            snprintf(error, error_size, "GPCM receive failed: %s", strerror(errno));

        close(sock);
        return false;
    }

    *response_ms = rr_gpcm_ticks_to_ms(gettime() - response_start);
    buffer[received] = '\0';

    if (strncmp(buffer,
                rr_gpcm_response_prefix,
                strlen(rr_gpcm_response_prefix)) != 0)
    {
        snprintf(error, error_size, "Unexpected GPCM response.");
        close(sock);
        return false;
    }

    close(sock);
    return true;
}

bool rr_gpcm_test_run(rr_gpcm_test_result_t *result)
{
    if (!result)
        return false;

    memset(result, 0, sizeof(*result));
    result->rounds = RR_GPCM_TEST_ROUNDS;

    struct hostent *host = gethostbyname(RR_GPCM_TEST_HOST);
    if (!host || !host->h_addr_list || !host->h_addr_list[0])
    {
        rr_gpcm_set_error(result, "DNS lookup failed.");
        return false;
    }

    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(RR_GPCM_TEST_PORT);
    memcpy(&server.sin_addr, host->h_addr_list[0], host->h_length);

    /* Store the actual address selected by DNS so the diagnostics screen
     * can show which Anycast address was resolved. */
    const unsigned char *ip = (const unsigned char *)&server.sin_addr;
    snprintf(result->resolved_ip, sizeof(result->resolved_ip),
             "%u.%u.%u.%u",
             (unsigned)ip[0], (unsigned)ip[1],
             (unsigned)ip[2], (unsigned)ip[3]);

    uint64_t connect_total = 0;
    uint64_t response_total = 0;
    uint32_t previous_response = 0;
    uint64_t jitter_total = 0;
    int response_samples = 0;
    char last_error[128] = "";

    for (int round = 0; round < RR_GPCM_TEST_ROUNDS; round++)
    {
        uint32_t connect_ms = 0;
        uint32_t response_ms = 0;
        char round_error[128] = "";

        if (!rr_gpcm_single_round(&server,
                                  &connect_ms,
                                  &response_ms,
                                  round_error,
                                  sizeof(round_error)))
        {
            snprintf(last_error, sizeof(last_error), "%s", round_error);
            continue;
        }

        result->successful_rounds++;
        connect_total += connect_ms;
        response_total += response_ms;

        if (response_samples > 0)
        {
            uint32_t delta = response_ms > previous_response
                                 ? response_ms - previous_response
                                 : previous_response - response_ms;
            jitter_total += delta;
        }

        previous_response = response_ms;
        response_samples++;

        /* Keep the most recent successful measurement available for
         * compatibility with the original single-round display. */
        result->connect_ms = connect_ms;
        result->response_ms = response_ms;
    }

    if (result->successful_rounds == 0)
    {
        rr_gpcm_set_error(result,
                          last_error[0] ? last_error : "All GPCM test rounds failed.");
        return false;
    }

    result->avg_connect_ms =
        (uint32_t)(connect_total / (uint64_t)result->successful_rounds);
    result->avg_response_ms =
        (uint32_t)(response_total / (uint64_t)result->successful_rounds);

    if (response_samples > 1)
    {
        result->jitter_ms =
            (uint32_t)(jitter_total / (uint64_t)(response_samples - 1));
    }

    if (result->successful_rounds < result->rounds)
    {
        rr_gpcm_set_error(result,
                          last_error[0] ? last_error : "One or more GPCM rounds failed.");
    }

    /* A completely successful set of rounds is SUCCESS. Partial success is
     * deliberately reported separately by the diagnostics UI as DEGRADED. */
    return result->successful_rounds == result->rounds;
}


enum rr_gpcm_quality rr_gpcm_test_quality(const rr_gpcm_test_result_t *result)
{
    if (!result || result->successful_rounds == 0)
        return RR_GPCM_QUALITY_FAILED;

    /* A partial result is deliberately capped even when its successful
     * samples have excellent latency. This keeps connection loss visible. */
    enum rr_gpcm_quality quality = RR_GPCM_QUALITY_EXCELLENT;

    if (result->avg_connect_ms >= 250 ||
        result->avg_response_ms >= 300 ||
        result->jitter_ms >= 75)
    {
        quality = RR_GPCM_QUALITY_BAD;
    }
    else if (result->avg_connect_ms >= 150 ||
             result->avg_response_ms >= 175 ||
             result->jitter_ms >= 40)
    {
        quality = RR_GPCM_QUALITY_POOR;
    }
    else if (result->avg_connect_ms >= 100 ||
             result->avg_response_ms >= 125 ||
             result->jitter_ms >= 20)
    {
        quality = RR_GPCM_QUALITY_FAIR;
    }
    else if (result->avg_connect_ms >= 50 ||
             result->avg_response_ms >= 75 ||
             result->jitter_ms >= 10)
    {
        quality = RR_GPCM_QUALITY_GOOD;
    }

    if (result->successful_rounds < result->rounds)
    {
        if (result->successful_rounds <= 2)
            return RR_GPCM_QUALITY_FAILED;

        if (result->successful_rounds == 3 && quality > RR_GPCM_QUALITY_POOR)
            quality = RR_GPCM_QUALITY_POOR;
        else if (result->successful_rounds == 4 && quality > RR_GPCM_QUALITY_FAIR)
            quality = RR_GPCM_QUALITY_FAIR;
    }

    return quality;
}

const char *rr_gpcm_quality_name(enum rr_gpcm_quality quality)
{
    switch (quality)
    {
        case RR_GPCM_QUALITY_EXCELLENT: return "EXCELLENT";
        case RR_GPCM_QUALITY_GOOD:      return "GOOD";
        case RR_GPCM_QUALITY_FAIR:      return "FAIR";
        case RR_GPCM_QUALITY_POOR:      return "POOR";
        case RR_GPCM_QUALITY_BAD:       return "BAD";
        case RR_GPCM_QUALITY_FAILED:
        default:                        return "FAILED";
    }
}
