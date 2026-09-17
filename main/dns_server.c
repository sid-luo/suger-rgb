#include "dns_server.h"

#include <errno.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define DNS_PORT 53
#define DNS_BUFFER_SIZE 512

static const char *TAG = "captive_dns";
static bool s_started;

static size_t dns_question_end(const uint8_t *packet, size_t length)
{
    if (length < 12) {
        return 0;
    }
    size_t position = 12;
    while (position < length && packet[position] != 0) {
        uint8_t label_length = packet[position];
        if ((label_length & 0xC0) != 0 || position + 1 + label_length > length) {
            return 0;
        }
        position += 1 + label_length;
    }
    if (position + 5 > length) {
        return 0;
    }
    return position + 5; // zero terminator + QTYPE + QCLASS
}

static void dns_task(void *arg)
{
    (void)arg;
    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0) {
        ESP_LOGE(TAG, "Could not create DNS socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        ESP_LOGE(TAG, "Could not bind DNS socket: errno %d", errno);
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }

    uint8_t packet[DNS_BUFFER_SIZE];
    while (true) {
        struct sockaddr_storage source;
        socklen_t source_length = sizeof(source);
        ssize_t received = recvfrom(socket_fd, packet, sizeof(packet), 0,
                                    (struct sockaddr *)&source, &source_length);
        if (received <= 0) {
            continue;
        }

        size_t question_end = dns_question_end(packet, received);
        if (question_end == 0 || question_end + 16 > sizeof(packet)) {
            continue;
        }

        // Standard response, recursion available, one answer.
        packet[2] = 0x81;
        packet[3] = 0x80;
        packet[6] = 0x00;
        packet[7] = 0x01;
        packet[8] = packet[9] = packet[10] = packet[11] = 0x00;

        const uint8_t answer[] = {
            0xC0, 0x0C,             // compressed name pointer
            0x00, 0x01,             // A record
            0x00, 0x01,             // IN class
            0x00, 0x00, 0x00, 0x3C, // 60 second TTL
            0x00, 0x04,
            192, 168, 4, 1,
        };
        memcpy(packet + question_end, answer, sizeof(answer));
        sendto(socket_fd, packet, question_end + sizeof(answer), 0,
               (struct sockaddr *)&source, source_length);
    }
}

esp_err_t dns_server_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    if (xTaskCreate(dns_task, "captive_dns", 3072, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}
