#pragma once
#include "esp_err.h"

/**
 * Wi-Fi + MQTT 配置 HTTP 服务器
 *
 * 路由列表：
 *   GET  /              配置主页（Tab: Wi-Fi 配置 | MQTT 配置）
 *   GET  /scan          返回 JSON 格式的周边 AP 列表
 *   POST /connect       提交 SSID 和密码，验证连接后保存到 NVS
 *   GET  /mqtt-config   读取 NVS 中保存的 MQTT 配置（JSON）
 *   POST /mqtt-config   保存 MQTT 配置到 NVS，并发送 EVT:MQTT_CFG 给 STM32
 *   GET  /generate_204        Captive Portal 检测（Android）
 *   GET  /hotspot-detect.html Captive Portal 检测（iOS/macOS）
 *   GET  /ncsi.txt            Captive Portal 检测（Windows）
 *   GET  /[*]                   兜底重定向（所有未知 URL）
 */

/** 启动 HTTP 服务器（必须在 Wi-Fi AP 模式启动后调用） */
esp_err_t http_server_start(void);

/** 停止 HTTP 服务器并释放资源 */
void http_server_stop(void);

/** 清除扫描结果 HTTP 缓存 */
void http_server_clear_scan_cache(void);
