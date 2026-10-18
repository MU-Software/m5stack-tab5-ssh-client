// SPDX-License-Identifier: GPL-3.0-or-later
#include "probe.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/poll.h>

#include <esp_random.h>
#include <esp_timer.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>

#include "c6_protocol.h"
#include "dns_message.h"
#include "sleep.h"

static const char* const public_addresses[] = C6_INTERNET_ADDRESSES;
static esp_netif_t* dead_dns;

void probe_dead_dns(esp_netif_t* netif) {
    dead_dns = netif;
}

int probe_bind(int fd, esp_netif_t* netif) {
    struct ifreq ifr = {0};
    if (netif == NULL || esp_netif_get_netif_impl_name(netif, ifr.ifr_name) != ESP_OK) return ENODEV;
    return setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof ifr) == 0 ? 0 : errno;
}

static bool addressed(esp_netif_t* netif) {
    esp_netif_ip_info_t ip;
    return netif != NULL && esp_netif_is_netif_up(netif) && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0;
}

// Begun and ended awake, with no light sleep between
static bool awake_since(unsigned sequence) {
    return sequence % 2 == 0 && sleep_sequence() == sequence;
}

int probe_connect(esp_netif_t* netif, uint32_t address, uint16_t port, int timeout_ms) {
    if (!addressed(netif)) return ENETDOWN;
    sleep_hold(true);
    unsigned sequence = sleep_sequence();
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = address};
    int rc = 0, fd = socket(AF_INET, SOCK_STREAM, 0);
    socklen_t length = sizeof rc;
    if (fd < 0)
        rc = errno;
    else if ((rc = probe_bind(fd, netif)) != 0 || fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
        if (rc == 0) rc = errno;
    } else if (connect(fd, (struct sockaddr*)&to, sizeof to) != 0 && (rc = errno) == EINPROGRESS) {
        if (poll(&(struct pollfd){.fd = fd, .events = POLLOUT}, 1, timeout_ms) != 1)
            rc = ETIMEDOUT;
        else if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &rc, &length) != 0)
            rc = errno;
    }
    if (fd >= 0) close(fd);
    if (!awake_since(sequence)) rc = EINTR;
    sleep_hold(false);
    return rc;
}

int probe_internet(esp_netif_t* netif, int per_address_ms, char address[16]) {
    int rc = ENETDOWN;
    for (size_t i = 0; i < sizeof public_addresses / sizeof *public_addresses; i++) {
        strlcpy(address, public_addresses[i], 16);
        rc = probe_connect(netif, inet_addr(public_addresses[i]), 443, per_address_ms);
        if (rc == ECONNREFUSED) rc = 0;
        if (rc == 0 || rc == EINTR || rc == ENETDOWN || rc == ENODEV) break;
    }
    return rc;
}

// The path's servers, then the public one
static size_t servers(esp_netif_t* netif, uint32_t list[3], size_t* path_count) {
    size_t n = 0;
    for (esp_netif_dns_type_t t = ESP_NETIF_DNS_MAIN; t <= ESP_NETIF_DNS_BACKUP; t++) {
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(netif, t, &dns) == ESP_OK && dns.ip.type == ESP_IPADDR_TYPE_V4 && dns.ip.u_addr.ip4.addr != 0)
            list[n++] = netif == dead_dns ? inet_addr("192.0.2.53") : dns.ip.u_addr.ip4.addr;
    }
    *path_count = n;
    list[n++] = inet_addr(public_addresses[0]);
    return n;
}

probe_lookup probe_resolve(esp_netif_t* netif, const char* name, int try_ms, uint32_t* address) {
    struct in_addr literal;
    if (inet_aton(name, &literal)) {
        *address = literal.s_addr;
        return PROBE_FOUND;
    }
    if (!addressed(netif)) return PROBE_DOWN;
    uint32_t list[3];
    size_t path_count, count = servers(netif, list, &path_count);
    uint8_t query[300];
    uint16_t id = (uint16_t)esp_random();
    size_t size = dns_query(id, name, query, sizeof query);
    if (size == 0) return PROBE_NO_NAME;
    sleep_hold(true);
    unsigned sequence = sleep_sequence();
    probe_lookup result = PROBE_NO_ANSWER;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in any = {.sin_family = AF_INET};
    if (fd >= 0 && probe_bind(fd, netif) == 0 && bind(fd, (struct sockaddr*)&any, sizeof any) == 0) {
        uint8_t message[512];
        bool denied = false;
        for (size_t i = 0; i < count && result == PROBE_NO_ANSWER; i++) {
            struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = list[i]};
            if (sendto(fd, query, size, 0, (struct sockaddr*)&to, sizeof to) < 0) continue;
            for (int64_t end = esp_timer_get_time() + try_ms * 1000LL, left; result == PROBE_NO_ANSWER && (left = end - esp_timer_get_time()) > 0;) {
                // In whole milliseconds, at least one: lwIP waits for ever on 0
                int64_t ms = (left + 999) / 1000;
                struct timeval wait = {.tv_sec = (time_t)(ms / 1000), .tv_usec = (suseconds_t)(ms % 1000 * 1000)};
                struct sockaddr_in from;
                socklen_t from_size = sizeof from;
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof wait);
                int got = recvfrom(fd, message, sizeof message, 0, (struct sockaddr*)&from, &from_size);
                if (got < 0) break;
                // From one of the servers asked so far, a late one included
                size_t k = 0;
                while (k <= i && list[k] != from.sin_addr.s_addr)
                    k++;
                if (k > i || from.sin_port != htons(53)) continue;
                dns_answer a = dns_parse(id, name, message, (size_t)got, address);
                if (a == DNS_FOUND) result = PROBE_FOUND;
                if (a == DNS_NO_NAME && k >= path_count) result = PROBE_NO_NAME;
                if (a == DNS_NO_NAME && k < path_count && !denied) {
                    // The public server once more, for a filtering resolver
                    denied = true;
                    i = path_count - 1;
                    break;
                }
                if (a == DNS_FAILED) break;
            }
        }
        if (result == PROBE_NO_ANSWER && denied) result = PROBE_NO_NAME;
    }
    if (fd >= 0) close(fd);
    if (!awake_since(sequence)) result = PROBE_SLEPT;
    sleep_hold(false);
    return result;
}
