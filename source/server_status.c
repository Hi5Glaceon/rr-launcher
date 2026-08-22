#include "server_status.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cacert_bin.h"
#include "console.h"
#include "pad.h"

#define RR_SERVER_STATUS_URL \
    "https://raw.githubusercontent.com/TeamWheelWizard/WheelWizard-Data/main/status.json"

struct rr_server_status_buffer
{
    char *data;
    size_t size;
};

typedef enum
{
    RR_SERVER_STATUS_NONE,
    RR_SERVER_STATUS_WARNING,
    RR_SERVER_STATUS_ERROR,
    RR_SERVER_STATUS_SUCCESS,
    RR_SERVER_STATUS_INFO,
    RR_SERVER_STATUS_PARTY,
    RR_SERVER_STATUS_QUESTION,
    RR_SERVER_STATUS_UNKNOWN
} rr_server_status_variant_t;

struct rr_server_status_result
{
    rr_server_status_variant_t variant;
    char message[256];
};

static struct rr_server_status_result rr_server_status_cached;
static bool rr_server_status_cached_valid = false;

static const char *rr_server_status_find_string(
    const char *json, const char *key)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = strstr(json, pattern);
    if (!p)
        return NULL;

    p += strlen(pattern);

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;

    if (*p != ':')
        return NULL;

    p++;

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;

    if (*p != '"')
        return NULL;

    return p + 1;
}

static bool rr_server_status_read_string(
    const char *json, const char *key, char *out, size_t out_size)
{
    const char *p = rr_server_status_find_string(json, key);

    if (!p || out_size == 0)
        return false;

    size_t out_len = 0;

    while (*p)
    {
        if (*p == '"')
        {
            out[out_len] = '\0';
            return true;
        }

        if (*p == '\\' && p[1] != '\0')
        {
            p++;

            switch (*p)
            {
                case '"':
                case '\\':
                case '/':
                    if (out_len + 1 < out_size)
                        out[out_len++] = *p;
                    break;

                case 'n':
                    if (out_len + 1 < out_size)
                        out[out_len++] = '\n';
                    break;

                case 'r':
                    if (out_len + 1 < out_size)
                        out[out_len++] = '\r';
                    break;

                case 't':
                    if (out_len + 1 < out_size)
                        out[out_len++] = '\t';
                    break;

                default:
                    if (out_len + 1 < out_size)
                        out[out_len++] = *p;
                    break;
            }

            p++;
            continue;
        }

        if (out_len + 1 >= out_size)
            return false;

        out[out_len++] = *p++;
    }

    return false;
}

static rr_server_status_variant_t rr_server_status_parse_variant(
    const char *variant)
{
    if (strcmp(variant, "none") == 0)
        return RR_SERVER_STATUS_NONE;
    if (strcmp(variant, "warning") == 0)
        return RR_SERVER_STATUS_WARNING;
    if (strcmp(variant, "error") == 0)
        return RR_SERVER_STATUS_ERROR;
    if (strcmp(variant, "success") == 0)
        return RR_SERVER_STATUS_SUCCESS;
    if (strcmp(variant, "info") == 0)
        return RR_SERVER_STATUS_INFO;
    if (strcmp(variant, "party") == 0)
        return RR_SERVER_STATUS_PARTY;
    if (strcmp(variant, "question") == 0)
        return RR_SERVER_STATUS_QUESTION;

    return RR_SERVER_STATUS_UNKNOWN;
}

static const char *rr_server_status_variant_name(
    rr_server_status_variant_t variant)
{
    switch (variant)
    {
        case RR_SERVER_STATUS_NONE:
            return "NONE";
        case RR_SERVER_STATUS_WARNING:
            return "WARNING";
        case RR_SERVER_STATUS_ERROR:
            return "ERROR";
        case RR_SERVER_STATUS_SUCCESS:
            return "SUCCESS";
        case RR_SERVER_STATUS_INFO:
            return "INFO";
        case RR_SERVER_STATUS_PARTY:
            return "PARTY";
        case RR_SERVER_STATUS_QUESTION:
            return "QUESTION";
        default:
            return "UNKNOWN";
    }
}

static bool rr_server_status_parse(
    const char *json, struct rr_server_status_result *result)
{
    char variant[32];

    if (!json || !result)
        return false;

    memset(result, 0, sizeof(*result));
    result->variant = RR_SERVER_STATUS_UNKNOWN;

    if (!rr_server_status_read_string(
            json, "variant", variant, sizeof(variant)))
        return false;

    if (!rr_server_status_read_string(
            json, "message", result->message, sizeof(result->message)))
        return false;

    result->variant = rr_server_status_parse_variant(variant);

    return result->variant != RR_SERVER_STATUS_UNKNOWN;
}

static size_t rr_server_status_write_callback(
    char *ptr,
    size_t size,
    size_t nmemb,
    void *userdata)
{
    struct rr_server_status_buffer *buffer =
        (struct rr_server_status_buffer *)userdata;

    size_t bytes = size * nmemb;

    char *new_data =
        realloc(buffer->data, buffer->size + bytes + 1);

    if (!new_data)
        return 0;

    buffer->data = new_data;

    memcpy(buffer->data + buffer->size, ptr, bytes);

    buffer->size += bytes;
    buffer->data[buffer->size] = '\0';

    return bytes;
}


static const char *rr_server_status_variant_color(
    rr_server_status_variant_t variant)
{
    switch (variant)
    {
        case RR_SERVER_STATUS_WARNING:
            return RRC_CON_ANSI_FG_BRIGHT_YELLOW;

        case RR_SERVER_STATUS_ERROR:
            return RRC_CON_ANSI_FG_BRIGHT_RED;

        case RR_SERVER_STATUS_INFO:
            return RRC_CON_ANSI_FG_BRIGHT_CYAN;

        case RR_SERVER_STATUS_PARTY:
            return RRC_CON_ANSI_FG_BRIGHT_MAGENTA;

        case RR_SERVER_STATUS_QUESTION:
            return RRC_CON_ANSI_FG_BRIGHT_YELLOW;

        case RR_SERVER_STATUS_NONE:
        case RR_SERVER_STATUS_SUCCESS:
        default:
            return RRC_CON_ANSI_FG_BRIGHT_GREEN;
    }
}

static void rr_server_status_print_centered(void)
{
    rrc_con_cursor_seek_to(0, 25);
    printf("Server Status\n\n");
}

static void rr_server_status_print_wrapped(const char *text, int width)
{
    const char *p = text;

    while (*p)
    {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;

        if (!*p)
            break;

        const char *line_start = p;
        const char *last_space = NULL;
        int len = 0;

        while (*p && *p != '\n' && *p != '\r')
        {
            if (*p == ' ' || *p == '\t')
                last_space = p;

            if (len >= width)
                break;

            p++;
            len++;
        }

        if (*p && *p != '\n' && *p != '\r' && len >= width && last_space)
        {
            int line_len = (int)(last_space - line_start);
            printf("%.*s\n", line_len, line_start);
            p = last_space + 1;
        }
        else
        {
            printf("%.*s\n", len, line_start);

            while (*p == '\n' || *p == '\r')
                p++;
        }
    }
}

static void rr_server_status_clear_screen(void)
{
    rrc_con_clear(true);

    for (int row = 0; row < rrc_con_get_rows(); row++)
        rrc_con_clear_line(row);

    rrc_con_cursor_seek_to(0, 0);
}

static void rr_server_status_wait_for_exit(void)
{
    while (1)
    {
        struct pad_state pad = rrc_pad_buttons();

        if (rrc_pad_home_pressed(pad) ||
            rrc_pad_a_pressed(pad) ||
            rrc_pad_b_pressed(pad))
            break;
            
        usleep(10000);
    }
}

static void rr_server_status_display_error(const char *message)
{
    rr_server_status_clear_screen();

    rr_server_status_print_centered();
    printf("\n%sERROR%s\n\n",
           RRC_CON_ANSI_FG_BRIGHT_RED,
           RRC_CON_ANSI_CLR);
    printf("%s\n", message);

    rr_server_status_wait_for_exit();
}

static bool rr_server_status_fetch_internal(bool display_errors)
{
    struct rr_server_status_buffer buffer;

    buffer.data = NULL;
    buffer.size = 0;

    CURL *curl = curl_easy_init();

    if (!curl)
    {
        rr_server_status_cached_valid = false;

        if (display_errors)
            rr_server_status_display_error("Could not initialize curl.");

        return false;
    }

    struct curl_blob ca_blob;

    ca_blob.data = (void *)cacert_bin;
    ca_blob.len = (size_t)cacert_bin_size;
    ca_blob.flags = 0;

    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca_blob);

    curl_easy_setopt(curl, CURLOPT_URL, RR_SERVER_STATUS_URL);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                    rr_server_status_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);

    CURLcode result = curl_easy_perform(curl);

    if (result != CURLE_OK)
    {
        char error_message[128];

        snprintf(error_message, sizeof(error_message),
                 "curl failed: %s",
                 curl_easy_strerror(result));

        curl_easy_cleanup(curl);
        free(buffer.data);

        rr_server_status_cached_valid = false;

        if (display_errors)
            rr_server_status_display_error(error_message);

        return false;
    }

    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    if (!buffer.data)
    {
        curl_easy_cleanup(curl);
        free(buffer.data);

        rr_server_status_cached_valid = false;

        if (display_errors)
            rr_server_status_display_error(
                "The server returned an empty response.");

        return false;
    }

    if (response_code < 200 || response_code >= 300)
    {
        char error_message[64];

        snprintf(error_message, sizeof(error_message),
                 "HTTP request failed with status %ld.",
                 response_code);

        curl_easy_cleanup(curl);
        free(buffer.data);

        rr_server_status_cached_valid = false;

        if (display_errors)
            rr_server_status_display_error(error_message);

        return false;
    }

    struct rr_server_status_result status;

    if (!rr_server_status_parse(buffer.data, &status))
    {
        curl_easy_cleanup(curl);
        free(buffer.data);

        rr_server_status_cached_valid = false;

        if (display_errors)
            rr_server_status_display_error(
                "The server returned an invalid status response.");

        return false;
    }

    curl_easy_cleanup(curl);
    free(buffer.data);

    rr_server_status_cached = status;
    rr_server_status_cached_valid = true;

    return true;
}

bool rr_server_status_fetch(void)
{
    return rr_server_status_fetch_internal(false);
}

bool rr_server_status_has_alert(void)
{
    return rr_server_status_cached_valid &&
           rr_server_status_cached.variant != RR_SERVER_STATUS_NONE;
}

const char *rr_server_status_alert_color(void)
{
    if (!rr_server_status_cached_valid)
        return RRC_CON_ANSI_FG_BRIGHT_GREEN;

    return rr_server_status_variant_color(rr_server_status_cached.variant);
}

static void rr_server_status_display_result(
    const struct rr_server_status_result *status)
{
    rr_server_status_clear_screen();

    rr_server_status_print_centered();
    printf("\n");

    rrc_con_cursor_seek_to(2, 24);
    printf("Variant: %s%s%s\n\n",
           rr_server_status_variant_color(status->variant),
           rr_server_status_variant_name(status->variant),
           RRC_CON_ANSI_CLR);

    printf("Message:\n");
    rr_server_status_print_wrapped(status->message, 72);

    rr_server_status_wait_for_exit();
}

bool rr_server_status_test(void)
{
    if (!rr_server_status_cached_valid)
    {
        if (!rr_server_status_fetch_internal(true))
            return false;
    }

    rr_server_status_display_result(&rr_server_status_cached);

    return true;
}
