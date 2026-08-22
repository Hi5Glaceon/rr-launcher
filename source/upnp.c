#include "upnp.h"
#include "private_port.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <stdlib.h>

#define SSDP_ADDR "239.255.255.250"
#define SSDP_PORT 1900
#define UPNP_BUF 8192

static rr_upnp_error_info_t rr_upnp_last_error_info;

static void rr_upnp_clear_last_error(void)
{
    memset(&rr_upnp_last_error_info, 0, sizeof(rr_upnp_last_error_info));
}

static void rr_upnp_set_no_device_error(const char *description,
                                        const char *solution)
{
    memset(&rr_upnp_last_error_info, 0, sizeof(rr_upnp_last_error_info));
    snprintf(rr_upnp_last_error_info.name,
             sizeof(rr_upnp_last_error_info.name),
             "No UPnP error code");
    snprintf(rr_upnp_last_error_info.description,
             sizeof(rr_upnp_last_error_info.description),
             "%s",
             description ? description : "No UPnP device error was returned.");
    snprintf(rr_upnp_last_error_info.solution,
             sizeof(rr_upnp_last_error_info.solution),
             "%s",
             solution ? solution : "Check the router connection and UPnP status.");
}

static bool rr_upnp_extract_soap_error(const char *response,
                                        rr_upnp_error_info_t *error_info)
{
    if (!response || !error_info)
        return false;

    const char *tag = strstr(response, "<errorCode>");

    if (!tag)
        tag = strstr(response, "<m:errorCode>");

    if (!tag)
        return false;

    const char *value = strchr(tag, '>');

    if (!value)
        return false;

    ++value;

    char *endptr = NULL;
    long code = strtol(value, &endptr, 10);

    if (endptr == value || code < 0 || code > 9999)
        return false;

    rr_upnp_error_lookup((int)code, error_info);
    return true;
}

const rr_upnp_error_info_t *rr_upnp_last_error(void)
{
    return &rr_upnp_last_error_info;
}


static bool rr_parse_location_url(const char *url, char *host, size_t host_sz,
                                  uint16_t *port, const char **path)
{
    const char *p = strstr(url, "://");
    if (!p) return false;
    p += 3;
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    const char *end = slash ? slash : p + strlen(p);
    if (colon && colon < end) end = colon;
    size_t n = (size_t)(end - p);
    if (!n || n >= host_sz) return false;
    memcpy(host, p, n); host[n] = 0;
    *port = 80;
    if (colon && colon < (slash ? slash : p + strlen(p))) {
        *port = (uint16_t)strtoul(colon + 1, NULL, 10);
    }
    *path = slash ? slash : "/";
    return true;
}

static bool rr_http_request(const char *host, uint16_t port, const char *path,
                            const char *extra_headers, const char *body,
                            char *response, size_t response_sz)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;
struct sockaddr_in addr;
memset(&addr, 0, sizeof(addr));
addr.sin_family = AF_INET;
addr.sin_port = htons(port);

if (inet_aton(host, &addr.sin_addr) == 0) {
    struct hostent *he = gethostbyname(host);
    if (!he) {
        close(s);
        return false;
    }

    memcpy(&addr.sin_addr, he->h_addr, sizeof(addr.sin_addr));
}
    struct timeval tv = { 4, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(s); return false; }

    size_t body_len = body ? strlen(body) : 0;
    char req[UPNP_BUF];
    int req_len = snprintf(req, sizeof(req),
        "%s %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n%s%s%s",
        body ? "POST" : "GET", path, host, (unsigned)port,
        extra_headers ? extra_headers : "", body ? "Content-Length: " : "",
        body ? "" : "");
    if (req_len < 0 || (size_t)req_len >= sizeof(req)) { close(s); return false; }
    if (body) {
        int add = snprintf(req + req_len, sizeof(req) - (size_t)req_len,
                           "%u\r\n\r\n%s", (unsigned)body_len, body);
        if (add < 0 || (size_t)(req_len + add) >= sizeof(req)) { close(s); return false; }
        req_len += add;
    } else {
        if ((size_t)req_len + 2 >= sizeof(req)) { close(s); return false; }
        req[req_len++] = '\r'; req[req_len++] = '\n';
    }
    if (send(s, req, req_len, 0) < 0) { close(s); return false; }

    size_t used = 0;
    while (used + 1 < response_sz) {
        int n = recv(s, response + used, response_sz - used - 1, 0);
        if (n <= 0) break;
        used += (size_t)n;
    }
    response[used] = 0;
    close(s);
    return used != 0;
}

static bool rr_extract_header(const char *buf, const char *name, char *out, size_t out_sz)
{
    const char *p = buf;
    size_t name_len = strlen(name);

    while (*p) {
        const char *line_end = strstr(p, "\r\n");
        if (!line_end)
            line_end = strchr(p, '\n');
        if (!line_end)
            break;

        size_t line_len = (size_t)(line_end - p);

        if (line_len > name_len &&
            strncasecmp(p, name, name_len) == 0 &&
            p[name_len] == ':') {
            const char *value = p + name_len + 1;

            while (*value == ' ' || *value == '\t')
                ++value;

            size_t n = (size_t)(line_end - value);
            if (n >= out_sz)
                n = out_sz - 1;

            memcpy(out, value, n);
            out[n] = 0;
            return true;
        }

        p = (*line_end == '\r' && line_end[1] == '\n')
                ? line_end + 2
                : line_end + 1;
    }

    return false;
}

static bool rr_fetch_control_url(rr_upnp_device_t *d)
{
    char host[64]; uint16_t port; const char *path;
    if (!rr_parse_location_url(d->location, host, sizeof(host), &port, &path)) return false;
    char xml[UPNP_BUF];
    if (!rr_http_request(host, port, path, NULL, NULL, xml, sizeof(xml))) return false;

    const char *p = xml;
    while ((p = strstr(p, "<service>")) != NULL) {
        const char *e = strstr(p, "</service>");
        if (!e) break;
        const char *st = strstr(p, "<serviceType>");
        const char *ct = strstr(p, "<controlURL>");
        if (st && ct && st < e && ct < e) {
            st += strlen("<serviceType>");
            ct += strlen("<controlURL>");
            const char *ste = strstr(st, "</serviceType>");
            const char *cte = strstr(ct, "</controlURL>");
            if (ste && cte && ste < e && cte < e) {
                size_t sn = (size_t)(ste - st);
                size_t cn = (size_t)(cte - ct);

                char service_type[256];
                bool is_wan_service = false;
                if (sn < sizeof(service_type)) {
                    memcpy(service_type, st, sn);
                    service_type[sn] = 0;
                    is_wan_service =
                        strstr(service_type, "WANIPConnection") != NULL ||
                        strstr(service_type, "WANPPPConnection") != NULL;
                }

                if (sn < sizeof(d->service_type) && cn < sizeof(d->control_url) &&
                    is_wan_service) {
                    memcpy(d->service_type, st, sn); d->service_type[sn] = 0;
                    char rel[256]; memcpy(rel, ct, cn); rel[cn] = 0;
                    if (rel[0] == '/') {
                        snprintf(d->control_url, sizeof(d->control_url), "http://%s:%u%s", host, (unsigned)port, rel);
                    } else {
                        const char *base = strrchr(path, '/');
                        size_t dir = base ? (size_t)(base - path + 1) : 1;
                        char full[256];
                        snprintf(full, sizeof(full), "%.*s%s", (int)dir, path, rel);
                        snprintf(d->control_url, sizeof(d->control_url), "http://%s:%u%s", host, (unsigned)port, full);
                    }
                    strncpy(d->host, host, sizeof(d->host)-1); d->port = port;
                    d->valid = true;
                    return true;
                }
            }
        }
        p = e + 9;
    }
    return false;
}

bool rr_upnp_discover(rr_upnp_device_t *device, uint32_t timeout_ms)
{
    if (!device) return false;
    memset(device, 0, sizeof(*device));
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return false;
    int yes = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst)); dst.sin_family = AF_INET; dst.sin_port = htons(SSDP_PORT);
    inet_aton(SSDP_ADDR, &dst.sin_addr);
    const char *msg =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 2\r\n"
        "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n";
    if (sendto(s, msg, strlen(msg), 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) { close(s); return false; }

    char buf[2048];
    uint32_t elapsed = 0;
    const uint32_t poll_ms = 100;

    while (elapsed < timeout_ms)
    {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(s, &readfds);

        uint32_t remaining = timeout_ms - elapsed;
        uint32_t wait_ms = remaining < poll_ms ? remaining : poll_ms;

        struct timeval wait_tv;
        wait_tv.tv_sec = wait_ms / 1000;
        wait_tv.tv_usec = (wait_ms % 1000) * 1000;

        int ready = select(s + 1, &readfds, NULL, NULL, &wait_tv);

        if (ready < 0)
        {
            close(s);
            return false;
        }

        if (ready == 0)
        {
            elapsed += wait_ms;
            continue;
        }

        if (FD_ISSET(s, &readfds))
        {
            int n = recvfrom(s, buf, sizeof(buf)-1, 0, NULL, NULL);

            if (n > 0)
            {
                buf[n] = 0;

                if (!rr_extract_header(buf, "LOCATION",
                                        device->location,
                                        sizeof(device->location)))
                {
                    close(s);
                    return false;
                }

                close(s);
                return rr_fetch_control_url(device);
            }
        }
    }

    close(s);
    return false;
}

static bool rr_local_ipv4(char *out, size_t out_sz)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return false;
    struct sockaddr_in remote;
    memset(&remote, 0, sizeof(remote)); remote.sin_family = AF_INET; remote.sin_port = htons(53);
    inet_aton("8.8.8.8", &remote.sin_addr);
    bool ok = connect(s, (struct sockaddr *)&remote, sizeof(remote)) == 0;
    struct sockaddr_in local; socklen_t len = sizeof(local);
    if (ok) ok = getsockname(s, (struct sockaddr *)&local, &len) == 0 &&
                 inet_ntop(AF_INET, &local.sin_addr, out, out_sz) != NULL;
    close(s); return ok;
}

static bool rr_parse_control_url(const rr_upnp_device_t *d, char *host, size_t host_sz,
                                 uint16_t *port, const char **path)
{
    return rr_parse_location_url(d->control_url, host, host_sz, port, path);
}

static bool rr_upnp_soap(const rr_upnp_device_t *d, const char *action, const char *body,
                         char *response, size_t response_sz)
{
    char host[64]; uint16_t port; const char *path;
    if (!rr_parse_control_url(d, host, sizeof(host), &port, &path)) return false;
    char headers[512];
    snprintf(headers, sizeof(headers),
        "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"%s#%s\"\r\n",
        d->service_type, action);
    return rr_http_request(host, port, path, headers, body, response, response_sz);
}

bool rr_upnp_add_udp_mapping(const rr_upnp_device_t *device, uint16_t external_port,
                             uint16_t internal_port, const char *description,
                             uint32_t lease_seconds)
{
    rr_upnp_clear_last_error();

    if (!device || !device->valid || !description)
    {
        rr_upnp_set_no_device_error(
            "The UPnP device information is invalid.",
            "Run discovery again and check that the router exposes UPnP IGD.");
        return false;
    }

    char client[32];

    if (!rr_local_ipv4(client, sizeof(client)))
    {
        rr_upnp_set_no_device_error(
            "The launcher's local IPv4 address could not be determined.",
            "Check the Wii's network connection.");
        return false;
    }
    char body[2048];
    snprintf(body, sizeof(body),
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:AddPortMapping xmlns:u=\"%s\">"
        "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort>"
        "<NewProtocol>UDP</NewProtocol><NewInternalPort>%u</NewInternalPort>"
        "<NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
        "<NewPortMappingDescription>%s</NewPortMappingDescription><NewLeaseDuration>%u</NewLeaseDuration>"
        "</u:AddPortMapping></s:Body></s:Envelope>",
        device->service_type, (unsigned)external_port, (unsigned)internal_port,
        client, description, (unsigned)lease_seconds);
    char response[UPNP_BUF];

    if (!rr_upnp_soap(device, "AddPortMapping", body, response, sizeof(response)))
    {
        rr_upnp_set_no_device_error(
            "The router did not return a valid UPnP response.",
            "Check that UPnP is enabled and that the router is reachable.");
        return false;
    }

    if (strstr(response, " 200 ") != NULL ||
        strstr(response, "HTTP/1.1 200") != NULL)
        return true;

    if (!rr_upnp_extract_soap_error(response, &rr_upnp_last_error_info))
        rr_upnp_set_no_device_error(
            "The router rejected the UPnP request without a readable SOAP error code.",
            "Check the router's UPnP settings and system log.");

    return false;
}

bool rr_upnp_delete_udp_mapping(const rr_upnp_device_t *device, uint16_t external_port,
                                uint16_t internal_port)
{
    rr_upnp_clear_last_error();

    (void)internal_port;
    if (!device || !device->valid) return false;
    char body[1024];
    snprintf(body, sizeof(body),
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:DeletePortMapping xmlns:u=\"%s\">"
        "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>"
        "</u:DeletePortMapping></s:Body></s:Envelope>", device->service_type, (unsigned)external_port);
    char response[UPNP_BUF];

    if (!rr_upnp_soap(device, "DeletePortMapping", body, response, sizeof(response)))
    {
        rr_upnp_set_no_device_error(
            "The router did not return a valid UPnP response.",
            "Check that UPnP is enabled and that the router is reachable.");
        return false;
    }

    if (strstr(response, " 200 ") != NULL ||
        strstr(response, "HTTP/1.1 200") != NULL)
        return true;

    if (!rr_upnp_extract_soap_error(response, &rr_upnp_last_error_info))
        rr_upnp_set_no_device_error(
            "The router rejected the UPnP delete request without a readable SOAP error code.",
            "Check the router's UPnP settings and system log.");

    return false;
}

bool rr_upnp_prepare_mkwii_mapping(uint16_t *port_out)
{
    rr_upnp_clear_last_error();

    if (!port_out)
    {
        rr_upnp_set_no_device_error(
            "The UPnP output parameter is invalid.",
            "This is an internal launcher error.");
        return false;
    }

    *port_out = 0;

    uint16_t port;

    if (!rr_get_mkwii_private_port(&port))
    {
        rr_upnp_set_no_device_error(
            "The local Mario Kart Wii UDP port could not be determined.",
            "Check the local network configuration.");
        return false;
    }

    rr_upnp_device_t device;

    if (!rr_upnp_discover(&device, 3000) || !device.valid)
    {
        rr_upnp_set_no_device_error(
            "No compatible UPnP Internet Gateway Device was discovered.",
            "Enable UPnP on the router and make sure the Wii is on the same LAN.");
        return false;
    }

    if (!rr_upnp_add_udp_mapping(
            &device, port, port, "Retro Rewind Launcher", 43200))
        return false;

    *port_out = port;
    return true;
}
