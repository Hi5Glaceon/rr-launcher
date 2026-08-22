#include "upnp_error_codes.h"
#include "upnp_error_codes.h"
#include "upnp_error_codes_bin.h"

#include <stdio.h>
#include <string.h>

static bool rr_json_read_string(const char *start, const char *end,
                                const char *key, char *out, size_t out_size)
{
    char pattern[96];
    int n;

    if (!start || !end || !key || !out || out_size == 0)
        return false;

    n = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(pattern))
        return false;

    const char *p = start;

    while (p < end)
    {
        const char *key_pos = strstr(p, pattern);
        if (!key_pos || key_pos >= end)
            return false;

        const char *q = key_pos + strlen(pattern);

        while (q < end && (*q == ' ' || *q == '\t' ||
                           *q == '\r' || *q == '\n'))
            ++q;

        if (q >= end || *q != ':')
        {
            p = key_pos + 1;
            continue;
        }

        ++q;

        while (q < end && (*q == ' ' || *q == '\t' ||
                           *q == '\r' || *q == '\n'))
            ++q;

        if (q >= end || *q != '"')
            return false;

        ++q;
        size_t used = 0;

        while (q < end)
        {
            if (*q == '"')
            {
                out[used] = '\0';
                return true;
            }

            if (*q == '\\' && q + 1 < end)
            {
                ++q;
                char decoded = *q;

                switch (*q)
                {
                    case '"':
                    case '\\':
                    case '/':
                        break;
                    case 'n': decoded = '\n'; break;
                    case 'r': decoded = '\r'; break;
                    case 't': decoded = '\t'; break;
                    default: break;
                }

                if (used + 1 >= out_size)
                    return false;

                out[used++] = decoded;
                ++q;
                continue;
            }

            if (used + 1 >= out_size)
                return false;

            out[used++] = *q++;
        }

        return false;
    }

    return false;
}

bool rr_upnp_error_lookup(int code, rr_upnp_error_info_t *result)
{
    if (!result)
        return false;

    memset(result, 0, sizeof(*result));
    result->code = code;

    if (code <= 0)
    {
        snprintf(result->name, sizeof(result->name), "No UPnP error code");
        snprintf(result->description, sizeof(result->description),
                 "The router did not return a UPnP SOAP error code.");
        snprintf(result->solution, sizeof(result->solution),
                 "Check the router connection and UPnP status.");
        return false;
    }

        const char *json = (const char *)upnp_error_codes_bin;
        const char *json_end = (const char *)upnp_error_codes_bin_end;

    char code_key[32];
    snprintf(code_key, sizeof(code_key), "\"%d\"", code);

    const char *p = json;

    while (p < json_end)
    {
        const char *key = strstr(p, code_key);

        if (!key || key >= json_end)
            break;

        const char *after = key + strlen(code_key);

        while (after < json_end && (*after == ' ' || *after == '\t' ||
                                    *after == '\r' || *after == '\n'))
            ++after;

        if (after < json_end && *after == ':')
        {
            const char *object_start = strchr(after, '{');

            if (!object_start || object_start >= json_end)
                break;

            const char *object_end = strchr(object_start, '}');

            if (!object_end || object_end > json_end)
                break;

            bool ok =
                rr_json_read_string(object_start, object_end, "name",
                                     result->name, sizeof(result->name)) &&
                rr_json_read_string(object_start, object_end, "description",
                                     result->description, sizeof(result->description)) &&
                rr_json_read_string(object_start, object_end, "solution",
                                     result->solution, sizeof(result->solution));

            if (ok)
            {
                result->known = true;
                return true;
            }

            break;
        }

        p = key + 1;
    }

    snprintf(result->name, sizeof(result->name), "Unknown UPnP error");
    snprintf(result->description, sizeof(result->description),
             "The router returned a UPnP error code that is not yet in the registry.");
    snprintf(result->solution, sizeof(result->solution),
             "Check the router documentation or add this code to upnp_error_codes.json.");

    return false;
}
