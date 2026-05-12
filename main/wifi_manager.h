#pragma once
#include "esp_err.h"
#include "esp_wifi.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * Wi-Fi Manager —— ESP-07S 配网模块
 *
 * 状态机：
 *   IDLE ──[START_AP]──> AP_RUNNING ──[用户提交]──> CONNECTING
 *                           ^                           |
 *                           |─────[失败]────────────────┘
 *                                   |─[成功]──> CONNECTED ──[重启]
 *
 * 关键修复说明：
 *   esp_wifi_start() 每次调用都会将 promiscuous_rx_cb 重置为 NULL。
 *   约 12s 后 ppTask 调用 NULL → binary blob 进入 15s 内部等待 → Task WDT。
 *   解决方案：每次 esp_wifi_start() 后立即重新注册空混杂回调。
 */

#define WIFI_MGR_MAX_AP  20   /**< 扫描结果最大保留条数 */

/** 初始化 Wi-Fi 驱动、NVS、事件循环 */
esp_err_t wifi_manager_init(void);

/**
 * 处理来自 STM32 的命令字符串
 * @param cmd 命令字符串，如 "CMD:START_AP"
 */
void wifi_manager_handle_command(const char *cmd);

/**
 * 尝试连接到指定 Wi-Fi（由 HTTP 服务器调用）
 * 连接成功后自动保存凭据到 NVS 并发送 UART 事件
 *
 * @param ssid      目标 SSID
 * @param password  密码（开放网络传 ""）
 * @param ip_out    输出 IP 字符串缓冲区（至少 16 字节）
 * @param ip_out_len 缓冲区长度
 * @return true 连接请求已接受（异步）
 */
bool wifi_manager_try_connect(const char *ssid, const char *password,
                              char *ip_out, int ip_out_len);

/** 返回当前是否已连接到 Wi-Fi */
bool wifi_manager_is_connected(void);

/** 获取当前 IP 地址字符串 */
const char *wifi_manager_get_ip(void);

/**
 * 获取扫描结果（由预扫描填充）
 * @param records 输出指向内部缓冲区的指针
 * @param num     输出 AP 数量
 * @return true 扫描已完成且有结果
 */
bool wifi_manager_get_scan_results(wifi_ap_record_t **records, uint16_t *num);

/** 触发重新扫描（AP 期间为安全策略，保持缓存不变） */
void wifi_manager_trigger_rescan(void);

/**
 * 查询异步连接结果（供 /result HTTP 端点轮询）
 * @param state   输出：0=未完成/pending，1=成功，2=失败
 * @param ip      输出：成功时填入 IP 字符串（至少 16 字节），否则空字符串
 * @param ip_len  ip 缓冲区长度
 */
void wifi_manager_get_connect_result(int *state, char *ip, int ip_len);
