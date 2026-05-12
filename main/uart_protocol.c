#include "uart_protocol.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* UART0 用于与 STM32 通信，UART1 用于调试日志输出 */
#define UART_NUM_STM32  UART_NUM_0
#define UART_NUM_LOG    UART_NUM_1

static const char *TAG = "UartProto";

/* UART1 日志重定向（将 ESP_LOG 逐字节输出到 GPIO2，避免干扰 STM32 通信） */
static int uart1_putchar(int ch)
{
    char c = (char)ch;
    uart_write_bytes(UART_NUM_LOG, &c, 1);
    return ch;
}

esp_err_t uart_protocol_init(void)
{
    /* ── 配置 UART1（GPIO2 = TX only）用于日志 ── */
    uart_config_t log_cfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(UART_NUM_LOG, &log_cfg);
    uart_driver_install(UART_NUM_LOG, 256, 256, 0, NULL, 0);

    /* 将 ESP_LOG 输出重定向到 UART1 */
    esp_log_set_putchar(uart1_putchar);

    /* ── 配置 UART0（GPIO1/GPIO3）用于 STM32 通信 ── */
    uart_config_t stm_cfg = {
        .baud_rate  = UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(UART_NUM_STM32, &stm_cfg);
    uart_driver_install(UART_NUM_STM32,
                        UART_BUF_SIZE * 2,
                        UART_BUF_SIZE * 2,
                        0, NULL, 0);

    ESP_LOGI(TAG, "UART 协议初始化完成 (baud=%d)", UART_BAUD_RATE);
    return ESP_OK;
}

/* ──────────────────────────────────────────────── */
/* 发送事件到 STM32                                  */
/* ──────────────────────────────────────────────── */
void uart_send_event(const char *event)
{
    char buf[UART_CMD_MAX_LEN + 8];
    int  len = snprintf(buf, sizeof(buf), "EVT:%s\r\n", event);
    uart_write_bytes(UART_NUM_STM32, buf, len);
}

void uart_send_eventf(const char *fmt, ...)
{
    char data[UART_CMD_MAX_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(data, sizeof(data), fmt, args);
    va_end(args);
    uart_send_event(data);
}

void uart_send_ack(bool ok)
{
    const char *msg = ok ? "ACK:OK\r\n" : "ACK:ERR\r\n";
    uart_write_bytes(UART_NUM_STM32, msg, strlen(msg));
}

/* ──────────────────────────────────────────────── */
/* 读取来自 STM32 的一行命令                          */
/* ──────────────────────────────────────────────── */
bool uart_read_command(char *buf, int buf_len, int timeout_ms)
{
    /* 接收缓冲区（跨调用保持状态） */
    static char  rx_buf[UART_CMD_MAX_LEN];
    static int   rx_len = 0;

    uint8_t      ch;
    TickType_t   start  = xTaskGetTickCount();
    TickType_t   ticks  = pdMS_TO_TICKS(timeout_ms > 0 ? timeout_ms : 1);

    while ((xTaskGetTickCount() - start) < ticks) {
        int got = uart_read_bytes(UART_NUM_STM32, &ch, 1, 0);
        if (got <= 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        if (ch == '\n') {
            /* 收到行尾：去除 \r，终止字符串 */
            if (rx_len > 0 && rx_buf[rx_len - 1] == '\r') {
                rx_len--;
            }
            rx_buf[rx_len] = '\0';

            if (rx_len > 0) {
                strncpy(buf, rx_buf, buf_len - 1);
                buf[buf_len - 1] = '\0';
                rx_len = 0;
                return true;
            }
        } else if (rx_len < UART_CMD_MAX_LEN - 1) {
            rx_buf[rx_len++] = (char)ch;
        } else {
            /* 缓冲区溢出，丢弃本行 */
            rx_len = 0;
        }
    }
    return false;
}
