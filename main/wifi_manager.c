#include "wifi_manager.h"
#include "uart_protocol.h"
#include "dns_server.h"
#include "http_server.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_system.h"
#include "tcpip_adapter.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

/* 外部全局标志：定义于 sha1-pbkdf2.c。
 * 声明为 volatile，防止编译器将同文件内的读取优化掉。
 * OPEN AUTH AP 模式时置 1，使 pbkdf2_sha1 立即返回，
 * 防止二进制驱动在 ppTask 上造成 WDT。 */
extern volatile int g_esp_open_auth_skip_pbkdf2;

#define TAG           "WiFiMgr"
#define NVS_NAMESPACE "wifi_cred"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PWD   "password"
#define AP_SSID_PREFIX "ESP07S-Cfg"   /* 最终为 ESP07S-Cfg-AABB */
#define AP_MAX_CONN    4
#define CONNECT_TIMEOUT_MS  15000     /* 连接超时 15 秒 */

/* ─── 事件组位 ─── */
#define BIT_CONNECTED  BIT0
#define BIT_FAILED     BIT1

/* ─── 内部状态 ─── */
static EventGroupHandle_t s_eg          = NULL;
static bool               s_connected   = false;
static char               s_ip[16]      = {0};
static char               s_ap_ssid[32] = {0};

/* ─── 扫描结果缓存（由预扫描填充） ─── */
static wifi_ap_record_t   s_ap_records[WIFI_MGR_MAX_AP];
static uint16_t           s_ap_num       = 0;
static bool               s_scan_ready   = false;  /* 扫描完成，待读取记录 */
static bool               s_records_ready = false; /* 已调用过 get_ap_records */
static bool               s_in_ap_mode   = false;  /* AP 配网期间不自动连接 STA */
static bool               s_is_connecting = false; /* 正在连接目标 AP，禁止此期间触发扫描 */
static volatile int       s_ap_clients     = 0;   /* AP 已连客户端数（仅供状态查询） */

/* ─── 异步连接任务（/connect 由此任务接力，切 APSTA 保持 AP 存活让浏览器可查 /result） ─── */
static char               s_pending_ssid[33] = {0};
static char               s_pending_pwd[65]  = {0};
static TaskHandle_t       s_connect_task     = NULL;

/* ─── 连接结果（供 /result HTTP 端点轮询，APSTA 保活期间填入） ─── */
static volatile int       s_connect_result_state = 0;   /* 0=pending, 1=成功, 2=失败 */
static char               s_connect_result_ip[16] = {0};
/* ─── 保存 AP 配置，切 APSTA 时需要恢复 AP 参数 ─── */
static wifi_config_t      s_saved_ap_cfg;

/* ❌已删除周期重扫任务：任何形式的 APSTA + 有客户连接时启动 STA 扫描，
 * 都会触发 ESP8266 RTOS SDK 驱动内部状态机崩溃。改为"启动前一次性
 * 纯 STA 阻塞扫描"方案 —— 扫描结果缓存后，AP 期间完全不再扫描。
 *
 * ❌进一步：AP 期间不再使用 APSTA 模式，改用纯 AP 模式。
 *   因为 APSTA 即便不主动扫描，驱动内部状态机长期运行仍会崩溃。
 *   /connect 请求到来后，由异步任务停 AP → 切 STA → 连接。 */

/* ──────────────────────────────────────────── */
/* Wi-Fi 系统事件处理（ESP8266 RTOS SDK 风格）   */
/* ──────────────────────────────────────────── */
static esp_err_t wifi_event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id) {

    case SYSTEM_EVENT_AP_START:
        ESP_LOGI(TAG, "AP 已就绪：%s", s_ap_ssid);
        break;

    case SYSTEM_EVENT_SCAN_DONE:
        s_scan_ready = true;
        ESP_LOGI(TAG, "扫描完成（待 httpd 任务读取结果）");
        break;

    case SYSTEM_EVENT_STA_START:
        /* 仅在非 AP 配网模式下才自动连接！
         * APSTA 模式下 esp_wifi_start() 会触发 STA_START。
         * 若在 AP 配网期间调用 esp_wifi_connect()，会导致 15s WDT。 */
        if (!s_in_ap_mode) {
            esp_wifi_connect();
        }
        break;

    case SYSTEM_EVENT_STA_GOT_IP:
        s_connected = true;
        snprintf(s_ip, sizeof(s_ip), IPSTR,
                 IP2STR(&event->event_info.got_ip.ip_info.ip));
        ESP_LOGI(TAG, "获取到 IP：%s", s_ip);
        xEventGroupSetBits(s_eg, BIT_CONNECTED);
        break;

    case SYSTEM_EVENT_STA_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "Wi-Fi 断开，原因：%d",
                 event->event_info.disconnected.reason);
        xEventGroupSetBits(s_eg, BIT_FAILED);
        break;

    case SYSTEM_EVENT_AP_STACONNECTED:
        s_ap_clients++;
        ESP_LOGI(TAG, "有设备连接到配网热点（共 %d 个）", s_ap_clients);
        uart_send_event("CLIENT_IN");
        break;

    case SYSTEM_EVENT_AP_STADISCONNECTED:
        if (s_ap_clients > 0) s_ap_clients--;
        ESP_LOGI(TAG, "设备离开配网热点（剩 %d 个）", s_ap_clients);
        break;

    default:
        break;
    }
    return ESP_OK;
}

/* ──────────────────────────────────────────── */
/* 空混杂模式回调——防止 ppTask 调用 NULL 指针崩溃  */
/*                                              */
/* 根本原因：esp_wifi_start() 每次调用都会将       */
/* promiscuous_rx_cb 重置为 NULL。约 12s 后       */
/* ppTask 调用 NULL → binary blob 进入内部 15s   */
/* 等待逻辑 → Task WDT（AP_START+27s 崩溃）。     */
/*                                              */
/* 解决方案：每次 esp_wifi_start() 后立即重新注册  */
/* ──────────────────────────────────────────── */
static void s_empty_promiscuous_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    /* 空实现：不使用混杂模式，仅为 ppTask 提供非 NULL 指针以防崩溃 */
    (void)buf; (void)type;
}

/* ──────────────────────────────────────────── */
/* 初始化                                        */
/* ──────────────────────────────────────────── */
esp_err_t wifi_manager_init(void)
{
    /* 初始化 TCP/IP 协议栈 */
    tcpip_adapter_init();

    /* 注册 Wi-Fi 事件处理函数 */
    ESP_ERROR_CHECK(esp_event_loop_init(wifi_event_handler, NULL));

    /* ★ 关键修复：在 esp_wifi_init() 之前就置 1，确保初始化期间驱动内部所有 pbkdf2 调用都走快速路径。
     * esp_wifi_init() 内部可能异步启动计时器，自证时开始至先为 1 应对所有情况。 */
    g_esp_open_auth_skip_pbkdf2 = 1;

    /* 初始化 Wi-Fi 驱动 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    /* 立即注册空混杂回调，防止 ppTask 调用 NULL 指针崩溃。
     * 关键：esp_wifi_start() 每次调用都会将该回调重置为 NULL。
     * 因此必须在每次 esp_wifi_start() 之后重新注册，否则 ~12s 后
     * ppTask 调用 NULL → 进入内部 15s 等待逻辑 → Task WDT 触发。
     * 此处是首次注册（wifi_init 时），后续每次 esp_wifi_start() 后再注册。 */
    esp_wifi_set_promiscuous_rx_cb(s_empty_promiscuous_cb);

    /* 创建事件组 */
    s_eg = xEventGroupCreate();

    ESP_LOGI(TAG, "Wi-Fi 驱动初始化完成");
    return ESP_OK;
}

/* ──────────────────────────────────────────── */
/* 启动 AP 配网模式（内部）                       */
/* ──────────────────────────────────────────── */
static void start_ap_mode(void)
{
    /* 获取 MAC 后 2 字节构造唯一 SSID */
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X",
             AP_SSID_PREFIX, mac[4], mac[5]);

    /* 初始化状态 */
    s_in_ap_mode     = true;
    s_is_connecting  = false;
    s_scan_ready     = false;
    s_records_ready  = false;
    s_ap_num         = 0;
    s_ap_clients     = 0;

    /* ═════ ① 纯 STA 模式阻塞扫描（最稳定） ═════ */
    ESP_LOGI(TAG, "预扫描：切纯 STA 模式...");
    esp_err_t stop_err = esp_wifi_stop();   /* 首次进入时可能未 start，忽略错误 */
    /* 停止后短暂等待，确保驱动状态机复位完成，再切模式/启动，
     * 避免紧接 start() 后 scan 立即发出时驱动尚未就绪而得到 0 个 AP。 */
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_err_t set_mode_err = esp_wifi_set_mode(WIFI_MODE_STA);
    esp_err_t err = esp_wifi_start();
    /* ★ 关键修复：esp_wifi_start() 会重置混杂回调为 NULL，必须在每次 start 后重新注册，
     * 否则约 12s 后 ppTask 调用 NULL 回调，进入内部 15s 等待逻辑导致 WDT。 */
    esp_wifi_set_promiscuous_rx_cb(s_empty_promiscuous_cb);
    /* 等待 STA 初始化完成：esp_wifi_start() 是异步的，立即 scan 会在驱动
     * 尚未就绪时发出探测帧，导致信道 dwell 时间极短而漏掉附近 AP。 */
    vTaskDelay(pdMS_TO_TICKS(200));
    /* 诊断：将每一步错误码输出到 UART（用户可见），方便定位执行失败位置 */
    uart_send_eventf("DBG:stop=%d,mode=%d,start=%d",
                     (int)stop_err, (int)set_mode_err, (int)err);

    /* 阻塞扫描，全信道扫描 2.4G
     * min=100ms / max=300ms：13 信道 × 300ms ≈ 3.9s，扫描质量远优于
     * 默认 0 值（某些 SDK 版本 0 → ~120ms/信道，容易漏掉附近 AP）。 */
    wifi_scan_config_t scan_cfg;
    memset(&scan_cfg, 0, sizeof(scan_cfg));
    scan_cfg.scan_type            = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.scan_time.active.min = 100;   /* ms */
    scan_cfg.scan_time.active.max = 300;   /* ms */
    esp_err_t sr = esp_wifi_scan_start(&scan_cfg, true);  /* block=true */
    if (sr == ESP_OK) {
        uint16_t n = WIFI_MGR_MAX_AP;
        esp_err_t rec_err = esp_wifi_scan_get_ap_records(&n, s_ap_records);
        uart_send_eventf("DBG:scan_ok,rec_err=%d,n=%u", (int)rec_err, (unsigned)n);
        if (rec_err == ESP_OK) {
            s_ap_num        = n;
            s_records_ready = true;
            s_scan_ready    = true;
            ESP_LOGI(TAG, "预扫描完成：%u 个 AP", n);
        } else {
            ESP_LOGW(TAG, "读取预扫描结果失败");
        }
    } else {
        uart_send_eventf("DBG:scan_fail,sr=%d", (int)sr);
        ESP_LOGW(TAG, "预扫描失败: %d（将继续启动 AP）", sr);
    }

    /* ═════ ② 停 STA 后切 AP 启动 ═════ */
    esp_wifi_stop();

    /* 显式停止 STA 侧 DHCP 客户端，防止其在 AP 期间持续发 DHCP discover
     * 触发驱动内部 NULL 回调崩溃 */
    tcpip_adapter_dhcpc_stop(TCPIP_ADAPTER_IF_STA);

    /* 配置 AP IP = 192.168.4.1 */
    tcpip_adapter_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip,      192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw,      192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP);
    tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_AP, &ip_info);
    tcpip_adapter_dhcps_start(TCPIP_ADAPTER_IF_AP);

    /* 配置 AP 参数（开放热点，无密码） */
    wifi_config_t ap_cfg;
    memset(&ap_cfg, 0, sizeof(ap_cfg));
    strlcpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.max_connection = AP_MAX_CONN;
    ap_cfg.ap.authmode       = WIFI_AUTH_OPEN;
    s_saved_ap_cfg = ap_cfg;   /* ★ 保存 AP 配置，供切 APSTA 时恢复 */

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    /* 在 esp_wifi_start() 前设置标志：
     * 二进制驱动在 AP 启动后可能立即开始内部定时器，
     * 确保它们调用 pbkdf2_sha1 时标志已生效。 */
    g_esp_open_auth_skip_pbkdf2 = 1;
    ESP_ERROR_CHECK(esp_wifi_start());
    /* ★ 关键修复：esp_wifi_start() 每次都会将混杂回调重置为 NULL。
     * 若不在此处重新注册，约 12s 后 ppTask 调用 NULL 回调，
     * binary blob 进入内部 15s 等待逻辑 → Task WDT（表现为 AP_START+27s 崩溃）。
     * 必须在每次 esp_wifi_start() 后立即重新注册！ */
    esp_wifi_set_promiscuous_rx_cb(s_empty_promiscuous_cb);
    /* 禁止省电模式，确保 AP 对客户端始终响应 */
    esp_wifi_set_ps(WIFI_PS_NONE);
    /* 注意：esp_wifi_set_inactive_time 必须在 esp_wifi_start() 之后调用 */
    esp_err_t it_err = esp_wifi_set_inactive_time(WIFI_IF_AP, 600);
    if (it_err != ESP_OK) {
        ESP_LOGW(TAG, "set_inactive_time 失败: %d", it_err);
    }

    ESP_LOGI(TAG, "配网热点已启动（纯 AP 模式）：%s", s_ap_ssid);

    /* 启动 DNS（Captive Portal）服务器 */
    dns_server_start(ip_info.gw);

    /* 启动 HTTP 配网服务器 */
    http_server_start();

    /* ═════ ③ AP 期间永不再扫描！彻底避开 SDK 内部 channel hop 崩溃 ═════ */

    /* 通知 STM32：AP 已启动 */
    uart_send_eventf("AP_START,%s", s_ap_ssid);
}

/* ──────────────────────────────────────────── */
/* 停止 AP 配网模式（内部）                       */
/* ──────────────────────────────────────────── */
static void stop_ap_mode(void)
{
    s_in_ap_mode    = false;  /* 允许 STA 自动连接恢复 */
    s_is_connecting = false;
    s_ap_clients    = 0;
    g_esp_open_auth_skip_pbkdf2 = 0;  /* 恢复：离开 AP 模式后恢复正常 PBKDF2 */

    http_server_stop();
    dns_server_stop();
    esp_wifi_stop();
    uart_send_event("AP_STOP");
    ESP_LOGI(TAG, "配网模式已停止");
}

/* ──────────────────────────────────────────── */
/* 异步连接任务：切 APSTA 保持 AP + HTTP 存活，          */
/* 连接完成后浏览器可通过 /result 轮询拿到真实 IP  */
/* ──────────────────────────────────────────── */
static void delayed_connect_task(void *arg)
{
    /* 先等 HTTP 响应发送完成，再切模式 */
    ESP_LOGI(TAG, "异步连接任务启动，等待 HTTP 响应发完...");
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* 重置连接结果（/result 返回 pending） */
    s_connect_result_state = 0;
    s_connect_result_ip[0] = '\0';

    ESP_LOGI(TAG, "切 APSTA 连接：%s（AP 保持存活让浏览器可查 /result）", s_pending_ssid);
    uart_send_eventf("CONNECTING,%s", s_pending_ssid);

    s_in_ap_mode    = false;   /* 解除 AP 标志，STA_START 会自动 connect */
    s_is_connecting = true;
    s_ap_clients    = 0;
    /* ★ WPA2 连接需要完整 PMK，临时允许 pbkdf2 全量计算 */
    g_esp_open_auth_skip_pbkdf2 = 0;

    wifi_config_t sta_cfg;
    memset(&sta_cfg, 0, sizeof(sta_cfg));
    strlcpy((char *)sta_cfg.sta.ssid,     s_pending_ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, s_pending_pwd,  sizeof(sta_cfg.sta.password));

    xEventGroupClearBits(s_eg, BIT_CONNECTED | BIT_FAILED);

    /* ★ 关键：不调用 esp_wifi_stop()/esp_wifi_start()！
     *   stop() 会立即关闭 AP，手机 Wi-Fi 断线 → Android Chrome 导航到"网络错误"
     *   页 → JS 上下文丢失 → 轮询永远不会执行。
     *   正确做法：用 esp_wifi_set_mode(APSTA) 动态切换模式，
     *   AP 全程不停，手机全程保持连接。 */

    /* 将 s_in_ap_mode 保持 true，阻止 STA_START 事件自动调用 esp_wifi_connect()
     * （模式切换时 STA_START 可能触发，我们改用手动时机调用 connect） */
    s_in_ap_mode = true;

    esp_wifi_set_mode(WIFI_MODE_APSTA);       /* AP 继续运行，增加 STA 模式 */
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    /* ★ 关键修复：start_ap_mode() 里调了 dhcpc_stop(STA)，必须重新启动 DHCP 客户端 */
    tcpip_adapter_dhcpc_start(TCPIP_ADAPTER_IF_STA);
    /* 稍等模式切换内部状态机稳定 */
    vTaskDelay(pdMS_TO_TICKS(300));
    /* 解除 AP 标志，允许 DISCONNECTED 事件正常设置 BIT_FAILED */
    s_in_ap_mode = false;
    /* 手动调用 esp_wifi_connect()，不依赖 STA_START 事件 */
    esp_wifi_connect();
    /* ★ 保险起见：若模式切换内部重置了混杂回调，重新注册 */
    esp_wifi_set_promiscuous_rx_cb(s_empty_promiscuous_cb);

    /* 等待连接结果，最多 15 秒 */
    EventBits_t bits = xEventGroupWaitBits(s_eg,
                                           BIT_CONNECTED | BIT_FAILED,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    s_is_connecting = false;

    if (bits & BIT_CONNECTED) {
        /* 连接成功 —— 保存凭据到 NVS */
        nvs_handle_t nvs;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_str(nvs, NVS_KEY_SSID, s_pending_ssid);
            nvs_set_str(nvs, NVS_KEY_PWD,  s_pending_pwd);
            nvs_commit(nvs);
            nvs_close(nvs);
            ESP_LOGI(TAG, "凭据已保存：%s", s_pending_ssid);
        }
        /* 写入连接结果，/result 端点将返回此 IP */
        strlcpy(s_connect_result_ip, s_ip, sizeof(s_connect_result_ip));
        s_connect_result_state = 1;
        /* 恢复：AP 开放认证无需 pbkdf2 */
        g_esp_open_auth_skip_pbkdf2 = 1;
        uart_send_eventf("CONNECTED,%s,%s", s_pending_ssid, s_ip);

        /* ★ 不停 HTTP/DNS/AP！
         *   项目要求：AP 必须持续存活直到收到显式 CMD:STOP_AP。
         *   用户在看到 Wi-Fi IP 后可能还需要配置 MQTT Tab，
         *   如果此处停掉 HTTP 服务器就无法操作 MQTT 配置页。
         *   CMD:STOP_AP 将调用 stop_ap_mode() 停止全部服务。 */
    } else {
        s_connect_result_state = 2;
        ESP_LOGW(TAG, "异步连接失败：%s", s_pending_ssid);
        uart_send_eventf("FAILED,%d", (int)(bits & BIT_FAILED ? 1 : 0));

        /* ★ 连接失败同样保持 AP + HTTP 存活：
         *   允许用户在同一页面重试填入密码或选择其他网络。 */
    }

    /* 任务结束：HTTP/DNS/AP 保持运行，等待 CMD:STOP_AP */
    s_connect_task = NULL;
    vTaskDelete(NULL);
}

/* ────────────────────────────────────────── */
/* 尝试连接 Wi-Fi（由 HTTP 服务器调用，异步版） */
/* 立即返回，由后台任务完成停 AP + 切 STA + 连接 */
/* ────────────────────────────────────────── */
bool wifi_manager_try_connect(const char *ssid, const char *password,
                              char *ip_out, int ip_out_len)
{
    if (!ssid || ssid[0] == '\0') return false;

    /* 已有连接任务运行，不重复启动 */
    if (s_connect_task != NULL) {
        ESP_LOGW(TAG, "已有连接任务正在运行，忽略本次请求");
        return true;
    }

    /* 保存待连接的凭据 */
    strlcpy(s_pending_ssid, ssid, sizeof(s_pending_ssid));
    strlcpy(s_pending_pwd,  password ? password : "", sizeof(s_pending_pwd));

    /* IP 空着——连接结果将通过 UART EVT 同步给 STM32 */
    if (ip_out && ip_out_len > 0) ip_out[0] = '\0';

    /* 启动后台任务接力 */
    BaseType_t ok = xTaskCreate(delayed_connect_task, "delayed_conn",
                                3072, NULL, 5, &s_connect_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "创建异步连接任务失败");
        s_connect_task = NULL;
        return false;
    }

    ESP_LOGI(TAG, "已接受配网请求，异步连接任务将于 2 秒后停 AP 切 STA");
    return true;   /* 立即返回 "已接受"，HTTP handler 于是正常响应 */
}

/* ──────────────────────────────────────────── */
/* 从 NVS 读取凭据并自动连接（内部）              */
/* ──────────────────────────────────────────── */
static bool connect_from_nvs(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "NVS 中无已保存凭据");
        return false;
    }

    char ssid[33]  = {0};
    char pwd[65]   = {0};
    size_t len;

    len = sizeof(ssid);
    if (nvs_get_str(nvs, NVS_KEY_SSID, ssid, &len) != ESP_OK) {
        nvs_close(nvs);
        return false;
    }
    len = sizeof(pwd);
    nvs_get_str(nvs, NVS_KEY_PWD, pwd, &len);
    nvs_close(nvs);

    ESP_LOGI(TAG, "从 NVS 读取到 Wi-Fi：%s，自动连接中...", ssid);
    uart_send_eventf("CONNECTING,%s", ssid);

    wifi_config_t sta_cfg;
    memset(&sta_cfg, 0, sizeof(sta_cfg));
    strlcpy((char *)sta_cfg.sta.ssid,     ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, pwd,  sizeof(sta_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    /* ★ WPA2 连接需要完整 PMK，临时允许 pbkdf2 全量计算（yield 每128轮不触发WDT） */
    g_esp_open_auth_skip_pbkdf2 = 0;
    ESP_ERROR_CHECK(esp_wifi_start());
    /* ★ 关键修复：每次 esp_wifi_start() 后必须重新注册混杂回调 */
    esp_wifi_set_promiscuous_rx_cb(s_empty_promiscuous_cb);

    xEventGroupClearBits(s_eg, BIT_CONNECTED | BIT_FAILED);
    EventBits_t bits = xEventGroupWaitBits(s_eg,
                                           BIT_CONNECTED | BIT_FAILED,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));

    if (bits & BIT_CONNECTED) {
        uart_send_eventf("CONNECTED,%s,%s", ssid, s_ip);
        return true;
    }

    uart_send_event("FAILED,nvs_connect");
    esp_wifi_stop();
    return false;
}

/* ──────────────────────────────────────────── */
/* 命令处理（由 app_main 调用）                   */
/* ──────────────────────────────────────────── */
void wifi_manager_handle_command(const char *cmd)
{
    ESP_LOGI(TAG, "收到命令：%s", cmd);

    if (strcmp(cmd, "CMD:START_AP") == 0) {
        start_ap_mode();
        uart_send_ack(true);

    } else if (strcmp(cmd, "CMD:STOP_AP") == 0) {
        stop_ap_mode();
        uart_send_ack(true);

    } else if (strcmp(cmd, "CMD:CLEAR") == 0) {
        nvs_handle_t nvs;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_erase_all(nvs);
            nvs_commit(nvs);
            nvs_close(nvs);
            ESP_LOGI(TAG, "Wi-Fi 凭据已清除");
        }
        uart_send_ack(true);

    } else if (strcmp(cmd, "CMD:STATUS") == 0) {
        const char *state = s_connected ? "CONNECTED" : "AP";
        uart_send_eventf("STATUS,%s,%s", state, s_ip);
        uart_send_ack(true);

    } else if (strcmp(cmd, "CMD:RESET") == 0) {
        uart_send_ack(true);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();

    } else if (strcmp(cmd, "CMD:AUTO_CONNECT") == 0) {
        /* 自动从 NVS 连接（上电时 STM32 可发此命令） */
        bool ok = connect_from_nvs();
        if (!ok) {
            uart_send_event("NO_CRED");
        }
        uart_send_ack(ok);

    } else {
        ESP_LOGW(TAG, "未知命令：%s", cmd);
        uart_send_ack(false);
    }
}

/* ──────────────────────────────────────────── */
/* 公共查询接口                                   */
/* ──────────────────────────────────────────── */
bool wifi_manager_is_connected(void)  { return s_connected; }
const char *wifi_manager_get_ip(void) { return s_ip; }

void wifi_manager_get_connect_result(int *state, char *ip, int ip_len)
{
    if (state)  *state = (int)s_connect_result_state;
    if (ip && ip_len > 0) {
        if (s_connect_result_state == 1)
            strlcpy(ip, s_connect_result_ip, ip_len);
        else
            ip[0] = '\0';
    }
}

bool wifi_manager_get_scan_results(wifi_ap_record_t **records, uint16_t *num)
{
    /* 若扫描完成且尚未读取，在此调用 esp_wifi_scan_get_ap_records()
     * 从 httpd 任务上下文被调用，栈大且不会与 Wi-Fi 任务死锁 */
    if (s_scan_ready && !s_records_ready) {
        uint16_t n = WIFI_MGR_MAX_AP;
        if (esp_wifi_scan_get_ap_records(&n, s_ap_records) == ESP_OK) {
            s_ap_num       = n;
            s_records_ready = true;
            ESP_LOGI(TAG, "AP 列表已读取，共 %u 条", n);
        }
        s_scan_ready = false;  /* 清除，防止重复读取 */
    }
    if (!s_records_ready || s_ap_num == 0) return false;
    *records = s_ap_records;
    *num     = s_ap_num;
    return true;
}

void wifi_manager_trigger_rescan(void)
{
    /* 稳定性方案：AP 期间不再主动扫描。需刷新列表请重启配网。 */
    ESP_LOGW(TAG, "AP 期间不支持重扫（安全策略），保持当前缓存");
}
