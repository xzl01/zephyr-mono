#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compile the real RSS HTTP client with deterministic network/Zephyr fakes.

The fake connect emulates a dual STA/AP host and rejects an unbound connection.
No network, SD writes, or firmware changes are made by this test.
"""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src/papermono_rss.c").read_text()


def function(name):
    match = re.search(
        r"^(?:static )?(?:const char\s*\*|int|bool|size_t)\s*"
        + name + r"\([^;]*?\)\n\{", SOURCE, re.MULTILINE,
    )
    if not match:
        raise AssertionError(f"missing C function: {name}")
    depth = 0
    for index in range(SOURCE.index("{", match.start()), len(SOURCE)):
        if SOURCE[index] == "{":
            depth += 1
        elif SOURCE[index] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[match.start():index + 1] + "\n"
    raise AssertionError(f"unclosed C function: {name}")


PREFIX = r'''
#define _POSIX_C_SOURCE 200112L
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#define net_in_addr in_addr
#define zsock_addrinfo addrinfo
#define NET_ADDR_PREFERRED 1
static char log_message[128];
#define RSS_LOG_INF(...) ((void)snprintf(log_message, sizeof(log_message), __VA_ARGS__))
#define RSS_LOG_WRN(...) ((void)snprintf(log_message, sizeof(log_message), __VA_ARGS__))
struct net_if { struct in_addr address; bool has_address; };
static struct net_if station, ap;
static struct net_if *station_iface, *default_iface;
static int bind_error, connect_error, socket_error;
static int sockets, binds, connects, closes, dns_calls;
static bool bound;
static size_t received;
static const char *http_response;
static char sent_request[512];
static struct net_if *net_if_get_wifi_sta(void) { return station_iface; }
static struct net_if *net_if_get_default(void) { return default_iface; }
static const struct in_addr *net_if_ipv4_get_global_addr(struct net_if *iface, int state) {
    assert(state == NET_ADDR_PREFERRED);
    return iface->has_address ? &iface->address : NULL;
}
static int net_addr_pton(int family, const char *text, void *out) {
    return inet_pton(family, text, out) == 1 ? 0 : -EINVAL;
}
static const char *net_addr_ntop(int family, const void *address, char *buffer, size_t size) {
    return inet_ntop(family, address, buffer, (socklen_t)size);
}
static int zsock_socket(int family, int type, int protocol) {
    assert(family == AF_INET && type == SOCK_STREAM && protocol == IPPROTO_TCP);
    ++sockets;
    if (socket_error) { errno = socket_error; return -1; }
    return 7;
}
static int zsock_bind(int sock, const struct sockaddr *address, socklen_t length) {
    const struct sockaddr_in *local = (const struct sockaddr_in *)address;
    assert(sock == 7 && length == sizeof(*local));
    assert(local->sin_family == AF_INET && local->sin_port == 0);
    /* AP is present and could win an unbound cross-subnet source selection. */
    assert(local->sin_addr.s_addr == station.address.s_addr);
    assert(local->sin_addr.s_addr != ap.address.s_addr);
    assert(connects == 0);
    ++binds;
    if (bind_error) { errno = bind_error; return -1; }
    bound = true;
    return 0;
}
static int zsock_connect(int sock, const struct sockaddr *address, socklen_t length) {
    const struct sockaddr_in *remote = (const struct sockaddr_in *)address;
    struct in_addr expected;
    inet_pton(AF_INET, "192.168.100.122", &expected);
    assert(sock == 7 && length == sizeof(*remote));
    assert(bound && binds == 1);
    assert(remote->sin_addr.s_addr == expected.s_addr && ntohs(remote->sin_port) == 8080);
    ++connects;
    if (connect_error) { errno = connect_error; return -1; }
    return 0;
}
static int zsock_close(int sock) { assert(sock == 7); ++closes; return 0; }
static ssize_t zsock_send(int sock, const void *buffer, size_t size, int flags) {
    assert(sock == 7 && bound && connects == 1 && !connect_error && flags == 0);
    assert(size < sizeof(sent_request));
    memcpy(sent_request, buffer, size); sent_request[size] = '\0';
    return (ssize_t)size;
}
static ssize_t zsock_recv(int sock, void *buffer, size_t size, int flags) {
    assert(sock == 7 && bound && connects == 1 && flags == 0);
    size_t available = strlen(http_response) - received;
    if (size > 17) size = 17; /* Fragment headers and body across reads. */
    if (size > available) size = available;
    memcpy(buffer, http_response + received, size); received += size;
    return (ssize_t)size;
}
static struct sockaddr_in dns_address;
static struct addrinfo dns_result;
static int zsock_getaddrinfo(const char *host, const char *port,
                            const struct addrinfo *hints, struct addrinfo **result) {
    assert(strcmp(host, "feed.test") == 0 && strcmp(port, "8080") == 0);
    assert(hints->ai_family == AF_INET && hints->ai_socktype == SOCK_STREAM);
    ++dns_calls;
    dns_address.sin_family = AF_INET; dns_address.sin_port = htons(8080);
    inet_pton(AF_INET, "192.168.100.122", &dns_address.sin_addr);
    dns_result.ai_addr = (struct sockaddr *)&dns_address;
    *result = &dns_result;
    return 0;
}
static void zsock_freeaddrinfo(struct addrinfo *result) { assert(result == &dns_result); }
'''

MAIN = r'''
static void clean(void) {
    memset(&station, 0, sizeof(station)); memset(&ap, 0, sizeof(ap));
    inet_pton(AF_INET, "192.168.8.44", &station.address);
    inet_pton(AF_INET, "192.168.4.1", &ap.address);
    station.has_address = ap.has_address = true;
    station_iface = &station; default_iface = &ap;
    bind_error = connect_error = socket_error = 0;
    sockets = binds = connects = closes = dns_calls = 0;
    bound = false; received = 0; sent_request[0] = log_message[0] = '\0';
    http_response = "HTTP/1.1 200 OK\r\nContent-Type: application/rss+xml\r\n\r\n<rss>latest</rss>";
}
static int fetch(const char *url, const char *expected) {
    char response[1024]; size_t offset = 0, length = 0; bool truncated = false;
    int ret = rss_http_get(url, response, sizeof(response), &offset, &length, &truncated);
    if (expected) {
        assert(ret == 0 && !truncated && length == strlen(expected));
        assert(memcmp(response + offset, expected, length) == 0);
    }
    return ret;
}
int main(void) {
    const char *url = "http://192.168.100.122:8080/feed.xml";
    clean();
    assert(fetch(url, "<rss>latest</rss>") == 0);
    assert(sockets == 1 && binds == 1 && connects == 1 && closes == 1);
    assert(strstr(sent_request, "GET /feed.xml HTTP/1.1\r\n") != NULL);
    assert(strstr(sent_request, "Host: 192.168.100.122:8080\r\n") != NULL);
    assert(strstr(log_message, "192.168.8.44 -> 192.168.100.122") != NULL);

    clean();
    station.has_address = false; /* AP's ready address is not a STA fallback. */
    assert(fetch(url, NULL) == -ENETDOWN);
    assert(sockets == 1 && !binds && !connects && closes == 1);
    clean(); station.address.s_addr = 0;
    assert(fetch(url, NULL) == -ENETDOWN);
    assert(!binds && !connects && closes == 1); /* Never bind INADDR_ANY. */
    clean(); station_iface = default_iface = NULL;
    assert(fetch(url, NULL) == -ENETDOWN);
    assert(!binds && !connects && closes == 1);

    clean(); bind_error = EADDRNOTAVAIL;
    assert(fetch(url, NULL) == -EADDRNOTAVAIL);
    assert(binds == 1 && !connects && closes == 1);
    clean(); connect_error = ETIMEDOUT;
    assert(fetch(url, NULL) == -ETIMEDOUT);
    assert(binds == 1 && connects == 1 && closes == 1);
    clean(); socket_error = EMFILE;
    assert(fetch(url, NULL) == -EMFILE);
    assert(sockets == 1 && !binds && !connects && !closes);

    clean();
    assert(fetch("http://feed.test:8080/feed.xml", "<rss>latest</rss>") == 0);
    assert(dns_calls == 1 && binds == 1 && connects == 1 && closes == 1);
    clean();
    http_response = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\n<rss>\r\nC\r\nlatest</rss>\r\n0\r\n\r\n";
    assert(fetch(url, "<rss>latest</rss>") == 0);
    assert(binds == 1 && connects == 1 && closes == 1);
    puts("RSS HTTP network: STA source binding, cleanup, DNS and response bytes: PASS");
    return 0;
}
'''


class RssNetworkTests(unittest.TestCase):
    def test_actual_http_client(self):
        defines = "\n".join(re.search(r"^#define " + name + r" .+$", SOURCE, re.MULTILINE).group(0)
                            for name in ("RSS_HOST_MAX", "RSS_PATH_MAX"))
        url_structure = re.search(r"^struct rss_url \{.*?^\};", SOURCE, re.MULTILINE | re.DOTALL).group(0)
        names = ("rss_iface", "rss_mem_find", "rss_url_parse", "rss_contains_ci", "rss_http_status",
                 "rss_dechunk", "rss_bind_source", "rss_http_get")
        # rss_iface returns a pointer rather than one of the extracted scalar types.
        iface = re.search(r"^static struct net_if \*rss_iface\(void\)\n\{.*?^\}",
                          SOURCE, re.MULTILINE | re.DOTALL).group(0)
        code = PREFIX + defines + "\n" + url_structure + "\n" + iface + "\n"
        code += "\n".join(function(name) for name in names if name != "rss_iface") + MAIN
        with tempfile.TemporaryDirectory(prefix="papermono-rss-network-") as directory:
            source = Path(directory) / "network.c"
            executable = Path(directory) / "network-test"
            source.write_text(code)
            subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
                "-std=c17", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
