// SPDX-License-Identifier: GPL-3.0-or-later
#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/poll.h>

#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <lwip/dns.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

static const char* TAG = "net";

static int64_t since(int64_t start) {
    return (esp_timer_get_time() - start) / 1000;
}

void net_check(const char* host, uint16_t port) {
    esp_netif_t* netif = esp_netif_get_default_netif();
    esp_netif_ip_info_t ip = {0};
    char address[16];
    if (netif != NULL) esp_netif_get_ip_info(netif, &ip);
    esp_ip4addr_ntoa(&ip.ip, address, sizeof address);
    ESP_LOGI(TAG, "default %s %s, DNS %s %s", netif != NULL ? esp_netif_get_desc(netif) : "none", address, ipaddr_ntoa(dns_getserver(0)),
             ipaddr_ntoa(dns_getserver(1)));
    char service[6];
    snprintf(service, sizeof service, "%u", port);
    struct addrinfo* ai;
    int64_t start = esp_timer_get_time();
    int rc = getaddrinfo(host, service, &(struct addrinfo){.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, &ai);
    if (rc != 0) {
        ESP_LOGW(TAG, "%s: no address (%d) after %lld ms", host, rc, since(start));
        return;
    }
    ESP_LOGI(TAG, "%s is %s after %lld ms", host, inet_ntoa(((struct sockaddr_in*)ai->ai_addr)->sin_addr), since(start));
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    start = esp_timer_get_time();
    if (fd >= 0 && fcntl(fd, F_SETFL, O_NONBLOCK) == 0 && connect(fd, ai->ai_addr, ai->ai_addrlen) != 0 && errno == EINPROGRESS) {
        socklen_t length = sizeof rc;
        if (poll(&(struct pollfd){.fd = fd, .events = POLLOUT}, 1, 20000) != 1)
            rc = ETIMEDOUT;
        else
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &rc, &length);
    } else {
        rc = errno;
    }
    ESP_LOGI(TAG, "TCP port %u: %s after %lld ms", port, rc == 0 ? "connected" : strerror(rc), since(start));
    if (fd >= 0) close(fd);
    freeaddrinfo(ai);
}
