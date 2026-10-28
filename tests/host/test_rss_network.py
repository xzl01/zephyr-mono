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
#include <sys/time.h>
#define net_in_addr in_addr
#define zsock_addrinfo addrinfo
#define zsock_timeval timeval
#define RSS_MIN(a,b) ((a) < (b) ? (a) : (b))
#define NET_ADDR_PREFERRED 1
static char log_message[128];
#define RSS_LOG_INF(...) ((void)snprintf(log_message, sizeof(log_message), __VA_ARGS__))
#define RSS_LOG_WRN(...) ((void)snprintf(log_message, sizeof(log_message), __VA_ARGS__))
struct net_if { struct in_addr address; bool has_address; };
static struct net_if station, ap;
static struct net_if *station_iface, *default_iface;
static int bind_error, connect_error, socket_error;
static int sockets, binds, connects, closes, dns_calls;
static int timeout_sets, recv_timeout_sets, send_timeout_sets, fail_timeout_at;
static int timeout_error, send_error, recv_error, dns_frees;
static int send_calls, recv_calls;
static int64_t now_ms, expected_deadline, recv_timeout_ms, send_timeout_ms, smallest_timeout;
static int64_t dns_delay_ms, connect_delay_ms, send_delay_ms, recv_delay_ms;
static size_t send_chunk, recv_chunk, sent_total;
static bool bound;
static size_t received;
static const char *http_response;
static char sent_request[512];
static int64_t k_uptime_get(void) { return now_ms; }
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
static int zsock_setsockopt(int sock, int level, int option, const void *value, socklen_t length) {
    const struct zsock_timeval *timeout = value;
    assert(sock == 7 && level == SOL_SOCKET && length == sizeof(*timeout));
    assert(timeout->tv_usec >= 0 && timeout->tv_usec < 1000000);
    int64_t milliseconds = timeout->tv_sec * 1000LL + timeout->tv_usec / 1000;
    assert(milliseconds > 0 && milliseconds <= 10000);
    assert(milliseconds <= expected_deadline - now_ms);
    ++timeout_sets;
    if (timeout_sets == fail_timeout_at) { errno = timeout_error; return -1; }
    if (milliseconds < smallest_timeout) smallest_timeout = milliseconds;
    if (option == SO_RCVTIMEO) { recv_timeout_ms = milliseconds; ++recv_timeout_sets; }
    else { assert(option == SO_SNDTIMEO); send_timeout_ms = milliseconds; ++send_timeout_sets; }
    return 0;
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
    assert(recv_timeout_sets == 1 && send_timeout_sets == 1);
    assert(remote->sin_addr.s_addr == expected.s_addr && ntohs(remote->sin_port) == 8080);
    ++connects;
    now_ms += connect_delay_ms;
    if (connect_error) { errno = connect_error; return -1; }
    return 0;
}
static int zsock_close(int sock) { assert(sock == 7); ++closes; return 0; }
static ssize_t zsock_send(int sock, const void *buffer, size_t size, int flags) {
    assert(sock == 7 && bound && connects == 1 && !connect_error && flags == 0);
    assert(send_timeout_sets >= 2);
    ++send_calls;
    now_ms += send_delay_ms < send_timeout_ms ? send_delay_ms : send_timeout_ms;
    if (send_error || (send_delay_ms && send_delay_ms >= send_timeout_ms)) {
        errno = send_error ? send_error : EAGAIN; return -1;
    }
    if (send_chunk && size > send_chunk) size = send_chunk;
    assert(sent_total + size < sizeof(sent_request));
    memcpy(sent_request + sent_total, buffer, size); sent_total += size;
    sent_request[sent_total] = '\0';
    return (ssize_t)size;
}
static ssize_t zsock_recv(int sock, void *buffer, size_t size, int flags) {
    assert(sock == 7 && bound && connects == 1 && flags == 0);
    assert(recv_timeout_sets > recv_calls + 1);
    ++recv_calls;
    now_ms += recv_delay_ms < recv_timeout_ms ? recv_delay_ms : recv_timeout_ms;
    if (recv_error || (recv_delay_ms && recv_delay_ms >= recv_timeout_ms)) {
        errno = recv_error ? recv_error : EAGAIN; return -1;
    }
    size_t available = strlen(http_response) - received;
    if (size > 17) size = 17; /* Fragment headers and body across reads. */
    if (recv_chunk && size > recv_chunk) size = recv_chunk;
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
    now_ms += dns_delay_ms;
    dns_address.sin_family = AF_INET; dns_address.sin_port = htons(8080);
    inet_pton(AF_INET, "192.168.100.122", &dns_address.sin_addr);
    dns_result.ai_addr = (struct sockaddr *)&dns_address;
    *result = &dns_result;
    return 0;
}
static void zsock_freeaddrinfo(struct addrinfo *result) { assert(result == &dns_result); ++dns_frees; }
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
    timeout_sets = recv_timeout_sets = send_timeout_sets = fail_timeout_at = 0;
    send_calls = recv_calls = dns_frees = 0;
    timeout_error = ENOPROTOOPT; send_error = recv_error = 0;
    now_ms = 5000; expected_deadline = now_ms + 30000;
    recv_timeout_ms = send_timeout_ms = 0; smallest_timeout = 10000;
    dns_delay_ms = connect_delay_ms = send_delay_ms = recv_delay_ms = 0;
    send_chunk = recv_chunk = sent_total = 0;
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

    /* Reaching the caller's fixed response capacity keeps the established
     * truncated-feed policy; time limits do not require a larger buffer. */
    clean();
    char large_response[512];
    const char *header = "HTTP/1.1 200 OK\r\n\r\n";
    memcpy(large_response, header, strlen(header));
    memset(large_response + strlen(header), 'x', sizeof(large_response) - strlen(header) - 1);
    large_response[sizeof(large_response) - 1] = '\0';
    http_response = large_response;
    char bounded_response[128];
    size_t offset = 0, length = 0; bool truncated = false;
    assert(rss_http_get(url, bounded_response, sizeof(bounded_response),
                        &offset, &length, &truncated) == 0);
    assert(truncated && offset == strlen(header));
    assert(length == sizeof(bounded_response) - 1 - offset && closes == 1);

    /* Both options are mandatory before connect: a platform/configuration
     * without timeout support must fail safely, never leave an unbounded socket. */
    for (int fail_at = 1; fail_at <= 2; ++fail_at) {
        clean(); fail_timeout_at = fail_at;
        assert(fetch(url, NULL) == -ENOPROTOOPT);
        assert(closes == 1 && !binds && !connects && !send_calls && !recv_calls);
    }
    clean(); fail_timeout_at = 3; timeout_error = EINVAL;
    assert(fetch(url, NULL) == -EINVAL);
    assert(connects == 1 && closes == 1 && !send_calls && !recv_calls);
    clean(); fail_timeout_at = 4; timeout_error = EINVAL;
    assert(fetch(url, NULL) == -EINVAL);
    assert(connects == 1 && closes == 1 && send_calls == 1 && !recv_calls);

    /* Kernel idle timeouts surface as EAGAIN on these blocking sockets. */
    clean(); recv_delay_ms = 11000;
    assert(fetch(url, NULL) == -ETIMEDOUT);
    assert(now_ms == 15000 && recv_calls == 1 && closes == 1);
    clean(); send_delay_ms = 11000;
    assert(fetch(url, NULL) == -ETIMEDOUT);
    assert(now_ms == 15000 && send_calls == 1 && !recv_calls && closes == 1);
    clean(); recv_error = ECONNRESET;
    assert(fetch(url, NULL) == -ECONNRESET && closes == 1);

    /* Small, ongoing progress cannot keep a source in front of other queued
     * sources forever. Remaining total budget also shortens the final idle wait. */
    clean(); recv_chunk = 1; recv_delay_ms = 9000;
    assert(fetch(url, NULL) == -ETIMEDOUT);
    assert(now_ms == expected_deadline && received == 3 && recv_calls == 4);
    assert(smallest_timeout == 3000 && closes == 1);
    clean(); send_chunk = 1; send_delay_ms = 9000;
    assert(fetch(url, NULL) == -ETIMEDOUT);
    assert(now_ms == expected_deadline && sent_total == 3 && send_calls == 4);
    assert(smallest_timeout == 3000 && !recv_calls && closes == 1);
    clean(); recv_error = EINTR; recv_delay_ms = 1000;
    assert(fetch(url, NULL) == -ETIMEDOUT);
    assert(now_ms == expected_deadline && closes == 1);

    /* DNS and connect consume the same total budget. A late result must not
     * enter HTTP send/receive or leak the resolver result/socket. */
    clean(); dns_delay_ms = 30000;
    assert(fetch("http://feed.test:8080/feed.xml", NULL) == -ETIMEDOUT);
    assert(dns_calls == 1 && dns_frees == 1 && !sockets && !connects);
    clean(); connect_delay_ms = 30000;
    assert(fetch(url, NULL) == -ETIMEDOUT);
    assert(connects == 1 && closes == 1 && !send_calls && !recv_calls);
    clean(); connect_delay_ms = 30000; connect_error = EIO;
    assert(fetch(url, NULL) == -ETIMEDOUT && closes == 1);

    /* The real Gold feed takes about six seconds to fill the bounded buffer. */
    clean(); recv_delay_ms = 1000;
    assert(fetch(url, "<rss>latest</rss>") == 0);
    assert(now_ms < expected_deadline && closes == 1);
    puts("RSS HTTP network: STA source, idle/total deadlines, cleanup, DNS and response bytes: PASS");
    return 0;
}
'''


class RssNetworkTests(unittest.TestCase):
    def test_actual_http_client(self):
        defines = "\n".join(re.search(r"^#define " + name + r" .+$", SOURCE, re.MULTILINE).group(0)
                            for name in ("RSS_HOST_MAX", "RSS_PATH_MAX", "RSS_HTTP_IDLE_TIMEOUT_MS", "RSS_HTTP_TOTAL_TIMEOUT_MS"))
        url_structure = re.search(r"^struct rss_url \{.*?^\};", SOURCE, re.MULTILINE | re.DOTALL).group(0)
        names = ("rss_iface", "rss_mem_find", "rss_url_parse", "rss_contains_ci", "rss_http_status",
                 "rss_dechunk", "rss_socket_timeout", "rss_bind_source", "rss_http_get")
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
