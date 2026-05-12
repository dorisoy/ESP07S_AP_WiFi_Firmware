#pragma once
#include "esp_err.h"
#include <stdbool.h>

/**
 * UART 通信协议 —— ESP-07S ↔ STM32F103
 *
 * 硬件连接：
 *   ESP-07S GPIO1 (TXD0)  →  STM32 USART2_RX (PA3)
 *   ESP-07S GPIO3 (RXD0)  →  STM32 USART2_TX (PA2)
 *   波特率：115200, 8N1
 *
 * ┌─────────────────────────────────────────────────────────────┐
 * │ 协议格式（文本行，\r\n 结尾）                                │
 * │  STM32 → ESP:  CMD:<name>[,<arg>]\r\n                       │
 * │  ESP → STM32:  EVT:<name>[,<data>...]\r\n                   │
 * │  ESP → STM32:  ACK:OK\r\n  /  ACK:ERR\r\n                  │
 * ├──────────────┬──────────────────────────────────────────────┤
 * │ 命令（CMD）  │ 说明                                          │
 * ├──────────────┼──────────────────────────────────────────────┤
 * │ START_AP     │ 启动 AP 配网模式                              │
 * │ STOP_AP      │ 停止 AP 配网模式                              │
 * │ CLEAR        │ 清除 NVS 中保存的 Wi-Fi 凭据                 │
 * │ STATUS       │ 查询当前连接状态                               │
 * │ RESET        │ 重启 ESP-07S 模块                             │
 * ├──────────────┬──────────────────────────────────────────────┤
 * │ 事件（EVT）  │ 格式                                          │
 * ├──────────────┼──────────────────────────────────────────────┤
 * │ READY        │ EVT:READY                                     │
 * │ AP_START     │ EVT:AP_START,<ssid>                           │
 * │ AP_STOP      │ EVT:AP_STOP                                   │
 * │ CLIENT_IN    │ EVT:CLIENT_IN                                 │
 * │ CONNECTING   │ EVT:CONNECTING,<ssid>                         │
 * │ CONNECTED    │ EVT:CONNECTED,<ssid>,<ip>                     │
 * │ FAILED       │ EVT:FAILED,<reason_code>                      │
 * │ STATUS       │ EVT:STATUS,<state>,<ssid>,<ip>                │
 * └──────────────┴──────────────────────────────────────────────┘
 *
 * <state>: IDLE / AP / CONNECTING / CONNECTED
 */

#define UART_BAUD_RATE   115200
#define UART_BUF_SIZE    512
#define UART_CMD_MAX_LEN 128

/**
 * 初始化 UART 协议模块
 * - UART0 (GPIO1/GPIO3) 用于与 STM32 通信
 * - 将 ESP 日志重定向到 UART1 (GPIO2, 仅 TX)，避免干扰 STM32 通信
 */
esp_err_t uart_protocol_init(void);

/** 发送事件到 STM32，格式：EVT:<event>\r\n */
void uart_send_event(const char *event);

/** 发送格式化事件到 STM32 */
void uart_send_eventf(const char *fmt, ...);

/** 发送 ACK 响应 */
void uart_send_ack(bool ok);

/**
 * 读取来自 STM32 的一行命令（阻塞，带超时）
 * @param buf        输出缓冲区
 * @param buf_len    缓冲区长度
 * @param timeout_ms 超时时间（毫秒），0 表示非阻塞
 * @return true 表示成功读到一条完整命令
 */
bool uart_read_command(char *buf, int buf_len, int timeout_ms);
