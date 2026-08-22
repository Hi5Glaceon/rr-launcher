#ifndef RR_GPCM_TEST_H
#define RR_GPCM_TEST_H

#include <stdbool.h>
#include <stdint.h>

#define RR_GPCM_TEST_HOST "gpcm.gs.play.rwfc.net"
#define RR_GPCM_TEST_PORT 29900
#define RR_GPCM_TEST_ROUNDS 5

typedef struct rr_gpcm_test_result
{
    uint32_t connect_ms;
    uint32_t response_ms;
    uint32_t avg_connect_ms;
    uint32_t avg_response_ms;
    uint32_t jitter_ms;
    int rounds;
    int successful_rounds;
    char resolved_ip[16];
    char error_message[128];
} rr_gpcm_test_result_t;

enum rr_gpcm_quality
{
    RR_GPCM_QUALITY_FAILED,
    RR_GPCM_QUALITY_BAD,
    RR_GPCM_QUALITY_POOR,
    RR_GPCM_QUALITY_FAIR,
    RR_GPCM_QUALITY_GOOD,
    RR_GPCM_QUALITY_EXCELLENT
};

bool rr_gpcm_test_run(rr_gpcm_test_result_t *result);

enum rr_gpcm_quality rr_gpcm_test_quality(const rr_gpcm_test_result_t *result);
const char *rr_gpcm_quality_name(enum rr_gpcm_quality quality);

#endif
