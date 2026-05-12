#include "wifi_manager.h"
#include "uart_protocol.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_task_wdt.h"

#define TAG "Main"

/* ─────────────────────────────────────────────────────────────────
 * Tick ISR 喂 WDT：vApplicationTickHook 在每个 FreeRTOS Tick 中断
 * (100 Hz = 每 10 ms) 里被调用，优先级高于任何 FreeRTOS 任务，
 * 即使 Wi-Fi ppTask 持续占用 CPU 也必然运行。
 *
 * 根因：esp_task_wdt_reset() 只在 idle hook 里调用；
 *       ppTask 持续 READY/RUNNING → idle 永不运行 → NMI WDT
 *       26.2 s 后触发 panic。
 *
 * 修复：每 500 tick (= 5 s) 从 Tick ISR 直接调用
 *       esp_task_wdt_reset()，将 s_nmi_wd_state 清零，
 *       同时喂硬件 WDT 计数器，彻底阻断崩溃路径。
 * ───────────────────────────────────────────────────────────────── */
void vApplicationTickHook(void)
{
    static uint32_t s_wdt_tick = 0;
    if (++s_wdt_tick >= 500) {   /* 500 ticks × 10 ms/tick = 5 s */
        s_wdt_tick = 0;
        esp_task_wdt_reset();    /* WDT_FEED() + s_nmi_wd_state = 0 */
    }
}

/**
 * ESP-07S Wi-Fi Manager 固件入口
 *
 * 启动流程：
 *  1. 初始化 NVS Flash
 *  2. 初始化 UART 协议（UART0 与 STM32 通信）
 *  3. 初始化 Wi-Fi Manager 模块
 *  4. 向 STM32 发送 READY 事件
 *  5. 主循环：持续接收并处理来自 STM32 的命令
 */
void app_main(void)
{
    /* ① 初始化 NVS Flash */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_LOGW(TAG, "NVS 无可用页，执行擦除...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ② 初始化 UART 协议（UART0: STM32 通信，UART1: 调试日志） */
    ESP_ERROR_CHECK(uart_protocol_init());

    /* ③ 初始化 Wi-Fi Manager */
    ESP_ERROR_CHECK(wifi_manager_init());

    /* ④ 通知 STM32：模块已就绪 */
    uart_send_event("READY");
    ESP_LOGI(TAG, "ESP-07S WiFiManager 固件就绪，等待 STM32 指令...");

    /* ⑤ 主循环：处理来自 STM32 的命令 */
    char cmd[UART_CMD_MAX_LEN];
    while (1) {
        if (uart_read_command(cmd, sizeof(cmd), 50)) {
            /* 收到一条完整命令，交给 wifi_manager 处理 */
            wifi_manager_handle_command(cmd);
        }
        /* 让出 CPU，避免空转 */
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
