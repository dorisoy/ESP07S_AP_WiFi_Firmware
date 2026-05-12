#pragma once
#include "esp_err.h"
#include "lwip/ip_addr.h"

/**
 * DNS 服务器（Captive Portal 实现）
 *
 * 启动后将所有 DNS 查询响应重定向到网关 IP，
 * 手机连接热点后系统会自动打开配网页面。
 */

/** 启动 DNS 服务器 */
esp_err_t dns_server_start(ip4_addr_t gateway_ip);

/** 停止 DNS 服务器 */
void dns_server_stop(void);
