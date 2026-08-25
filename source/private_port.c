#include "private_port.h"

#include <gccore.h>
#include <ogc/isfs.h>

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define RR_SETTING_PATH \
    "/title/00000001/00000002/data/setting.txt"

#define RR_SETTING_SIZE 256
#define RR_SETTING_XOR_KEY 0x73B5DBFAu

static void rr_setting_decrypt(uint8_t *buf)
{
    uint32_t key = RR_SETTING_XOR_KEY;

    for (int i = 0; i < RR_SETTING_SIZE; ++i)
    {
        buf[i] ^= (uint8_t)(key & 0xffu);
        key = (key << 1) | (key >> 31);
    }
}

static bool rr_parse_serno(
    const uint8_t *buf,
    uint32_t *serno)
{
    static const char prefix[] = "SERNO=";

    for (int i = 0; i <= RR_SETTING_SIZE - 6; ++i)
    {
        if (memcmp(buf + i, prefix, 6) != 0)
            continue;

        const char *p = (const char *)(buf + i + 6);

        uint32_t value = 0;
        int digits = 0;

        while (i + 6 + digits < RR_SETTING_SIZE)
        {
            char c = p[digits];

            if (c < '0' || c > '9')
                break;

            value = value * 10u + (uint32_t)(c - '0');
            digits++;
        }

        if (digits == 0)
            return false;

        *serno = value;
        return true;
    }

    return false;
}

bool rr_get_mkwii_private_port(uint16_t *port)
{
    if (!port)
        return false;

    if (ISFS_Initialize() < 0)
        return false;

    static uint8_t buf[RR_SETTING_SIZE] ATTRIBUTE_ALIGN(32);

    int fd = ISFS_Open(RR_SETTING_PATH, ISFS_OPEN_READ);

    if (fd < 0)
        return false;

    int result = ISFS_Read(fd, buf, RR_SETTING_SIZE);

    ISFS_Close(fd);

    if (result != RR_SETTING_SIZE)
        return false;

    rr_setting_decrypt(buf);

    uint32_t serno;

    if (!rr_parse_serno(buf, &serno))
        return false;

    *port = (uint16_t)(22000u + (serno % 1000u));

    return true;
}
