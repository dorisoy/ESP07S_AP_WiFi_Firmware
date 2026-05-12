#include "dns_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "tcpip_adapter.h"
#include "esp_log.h"
#include <string.h>

#define TAG          "DnsServer"
#define DNS_PORT     53
#define DNS_BUF_SIZE 512

static int            s_fd      = -1;
static TaskHandle_t   s_task    = NULL;
static ip4_addr_t     s_gateway;
static volatile bool  s_running = false;

/* ──────────────────────────────────── */
/* DNS 查询处理任务                      */
/* ──────────────────────────────────── */
static void dns_task(void *arg)
{
    char buf[DNS_BUF_SIZE];
    struct sockaddr_in client;
    socklen_t client_len = sizeof(client);

    while (s_running) {
        int len = recvfrom(s_fd, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client, &client_len);
        if (len < 0) {
            if (!s_running) break;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (len < 12) continue;  /* DNS 报文最小 12 字节 */

        /* ── 构造 DNS 应答报文，全部指向网关 IP ── */
        buf[2] |= 0x80;  /* QR = 1 (Response) */
        buf[3] |= 0x80;  /* RA = 1 */
        buf[7]  = 1;     /* ANCOUNT = 1 */

        /* 附加一条 A 记录 */
        memcpy(&buf[len],      "\xc0\x0c", 2);                               /* 名称压缩指针 */
        memcpy(&buf[len + 2],  "\x00\x01", 2);                               /* TYPE = A    */
        memcpy(&buf[len + 4],  "\x00\x01", 2);                               /* CLASS = IN  */
        memcpy(&buf[len + 6],  "\x00\x00\x00\x3c", 4);                       /* TTL = 60s   */
        memcpy(&buf[len + 10], "\x00\x04", 2);                               /* RDLENGTH = 4*/
        memcpy(&buf[len + 12], &s_gateway.addr, 4);                          /* IP 地址     */
        len += 16;

        sendto(s_fd, buf, len, 0,
               (struct sockaddr *)&client, client_len);
    }

    vTaskDelete(NULL);
}

/* ──────────────────────────────────── */
/* 公共接口                              */
/* ──────────────────────────────────── */
esp_err_t dns_server_start(ip4_addr_t gateway_ip)
{
    s_gateway = gateway_ip;

    s_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_fd < 0) {
        ESP_LOGE(TAG, "创建 socket 失败");
        return ESP_FAIL;
    }

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(DNS_PORT),
    };
    if (bind(s_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "绑定端口 53 失败");
        close(s_fd);
        s_fd = -1;
        return ESP_FAIL;
    }

    s_running = true;
    xTaskCreate(dns_task, "dns_srv", 2048, NULL, 5, &s_task);

    ESP_LOGI(TAG, "DNS 服务器已启动，重定向至 " IPSTR,
             IP2STR(&s_gateway));
    return ESP_OK;
}

void dns_server_stop(void)
{
    s_running = false;
    if (s_fd >= 0) {
        shutdown(s_fd, SHUT_RDWR);
        close(s_fd);
        s_fd = -1;
    }
    /* 等待任务退出 */
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "DNS 服务器已停止");
}
