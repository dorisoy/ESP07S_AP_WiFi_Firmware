#include "http_server.h"
#include "wifi_manager.h"
#include "uart_protocol.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

#define TAG          "HttpServer"
#define NVS_NS_MQTT  "mqtt_cfg"

static httpd_handle_t s_server = NULL;

/* ─ 扫描结果缓存：首次取到后就不再消耗缓冲，唠次直接返回缓存 ─ */
static char s_scan_cache[4096] = "[]";
static bool s_scan_cached      = false;

/* — Captive Portal 完成标志：页面加载后置为 true，/generate_204 就返回 204 防 Android 自动断线 — */
static bool s_portal_done = false;

/* ──────────────────────────────────────────────────────── */
/* 配网主页 HTML（Tab 布局：Wi-Fi 配置 | MQTT 配置）        */
/* ──────────────────────────────────────────────────────── */
static const char INDEX_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>CADS 设备配置</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    ".btn-sm{padding:7px 14px;background:#e3f2fd;color:#1976d2;border:1px solid #90caf9;"
    "  border-radius:6px;font-size:13px;cursor:pointer;margin-bottom:10px;width:auto}"
    ".btn-sm:active{background:#bbdefb}"
    "html,body{height:100%;width:100%}"
    "body{font-family:Arial,sans-serif;background:#f5f5f5;font-size:15px;"
    "  display:flex;flex-direction:column;min-height:100vh}"
    ".hdr{background:#1976d2;color:#fff;padding:14px 16px 10px;flex-shrink:0}"
    ".hdr h1{font-size:20px;font-weight:bold}"
    ".hdr p{font-size:13px;opacity:.8;margin-top:2px}"
    ".tabs{display:flex;background:#fff;border-bottom:2px solid #e0e0e0;"
    "  padding:0 12px;flex-shrink:0;position:sticky;top:0;z-index:10;"
    "  box-shadow:0 1px 3px rgba(0,0,0,.1)}"
    ".tab{padding:14px 20px;cursor:pointer;font-size:15px;"
    "  border-bottom:3px solid transparent;margin-bottom:-2px;color:#555}"
    ".tab.on{color:#1976d2;border-bottom-color:#1976d2;font-weight:bold}"
    ".page{display:none;padding:18px 16px;flex:1;width:100%}"
    ".page.on{display:block}"
    "h3{font-size:17px;font-weight:bold;margin-bottom:14px}"
    "label{font-size:14px;color:#555;display:block;margin:12px 0 4px}"
    "input[type=text],input[type=password],input[type=number]{"
    "  width:100%;padding:10px 12px;border:1px solid #ccc;"
    "  border-radius:6px;font-size:15px;background:#fff}"
    ".btn{width:100%;margin-top:18px;padding:13px;background:#1976d2;"
    "  color:#fff;border:none;border-radius:6px;font-size:16px;cursor:pointer}"
    ".btn:active{background:#1565c0}"
    ".msg{margin-top:14px;padding:10px 12px;border-radius:6px;"
    "  display:none;font-size:14px;text-align:center}"
    ".ok{background:#e8f5e9;color:#2e7d32}"
    ".err{background:#ffebee;color:#c62828}"
    ".info{background:#e3f2fd;color:#1565c0}"
    "hr{border:none;border-top:1px solid #e0e0e0;margin:18px 0}"
    ".scan-tip{font-size:14px;color:#555;margin-bottom:10px}"
    ".ap-item{display:block;padding:9px 4px;color:#1976d2;"
    "  text-decoration:none;font-size:14px;border-bottom:1px solid #f0f0f0}"
    ".ap-item:last-child{border-bottom:none}"
    "</style></head><body>"

    /* ── 顶部标题栏 ── */
    "<div class='hdr'><h1>&#x1F4F6; CADS 设备配置</h1></div>"

    /* ── 标签栏 ── */
    "<div class='tabs'>"
    "<div class='tab on' id='t0' onclick='sw(0)'>Wi-Fi 配置</div>"
    "<div class='tab'    id='t1' onclick='sw(1)'>MQTT 配置</div>"
    "</div>"

    /* ══════════ Tab 0: Wi-Fi 配置 ══════════ */
    "<div class='page on' id='p0'>"
    "<h3>新的 Wi-Fi</h3>"
    "<label>SSID:</label>"
    "<input type='text' id='ssid' placeholder='请选择或手动输入'>"
    "<label>密码:</label>"
    "<input type='password' id='pwd' placeholder='开放网络请留空'>"
    "<button class='btn' onclick='doConn()'>连接</button>"
    "<div class='msg' id='wmsg'></div>"
    "<hr>"
    "<p class='scan-tip'>\u4ece\u4e0b\u9762\u5217\u8868\u9009\u62e9 2.4G Wi-Fi:</p>"
    "<button class='btn-sm' id='rbtn' onclick='doRescan()'>&#x1F504; \u5237\u65b0\u5217\u8868</button>"
    "<div class='msg' id='smsg'></div>"
    "<div id='aps'><p style='color:#999;font-size:14px'>\u6b63\u5728\u626b\u63cf...</p></div>"
    "</div>"

    /* ══════════ Tab 1: MQTT 配置 ══════════ */
    "<div class='page' id='p1'>"
    "<h3>MQTT 服务器</h3>"
    "<label>服务器地址:</label>"
    "<input type='text' id='mhost' placeholder='192.168.1.100 或 mqtt.example.com'>"
    "<label>端口:</label>"
    "<input type='number' id='mport' value='1883' min='1' max='65535'>"
    "<label>客户端 ID:</label>"
    "<input type='text' id='mcid' placeholder='CADS_Device_001'>"
    "<label>用户名:</label>"
    "<input type='text' id='muser' placeholder='无需认证请留空'>"
    "<label>密码:</label>"
    "<input type='password' id='mpass' placeholder='无需认证请留空'>"
    "<label>订阅主题 (Subscribe):</label>"
    "<input type='text' id='msub' placeholder='例: device/CADS001/cmd'>"
    "<label>发布主题 (Publish):</label>"
    "<input type='text' id='mpub' placeholder='例: device/CADS001/data'>"
    "<button class='btn' onclick='saveMqtt()'>保存</button>"
    "<div class='msg' id='mmsg'></div>"
    "</div>"

    /* ══════════ JavaScript ══════════ */
    "<script>"
    /* Tab 切换 */
    "function sw(i){"
    "  [0,1].forEach(function(n){"
    "    document.getElementById('t'+n).className='tab'+(n==i?' on':'');"
    "    document.getElementById('p'+n).className='page'+(n==i?' on':'');"
    "  });"
    "}"

    /* 页面加载：通知 Android 门户已完成 + Wi-Fi 扫描 + MQTT 读取 */
    "window.onload=function(){"
    "  fetch('/captive-done');"
    "  loadScan();"
    "  fetch('/mqtt-config').then(function(r){return r.json()}).then(function(d){"
    "    if(d.host)document.getElementById('mhost').value=d.host;"
    "    if(d.port)document.getElementById('mport').value=d.port;"
    "    if(d.client_id)document.getElementById('mcid').value=d.client_id;"
    "    if(d.username)document.getElementById('muser').value=d.username;"
    "    if(d.sub_topic)document.getElementById('msub').value=d.sub_topic;"
    "    if(d.pub_topic)document.getElementById('mpub').value=d.pub_topic;"
    "  }).catch(function(){});"
    "};"

    /* 加载/显示扫描列表，扫描未就绪则轮询重试（最多 10 次，每次间隔 2 秒） */
    "function loadScan(retries){"
    "  retries=retries||0;"
    "  fetch('/scan').then(function(r){return r.json()}).then(function(list){"
    "    if((!list||!list.length)&&retries<10){"
    "      var tip='\u626b\u63cf\u4e2d\uff0c\u8bf7\u7a0d\u5019...('+(retries+1)+'/10)';"
    "      document.getElementById('aps').innerHTML='<p style=\"color:#999;font-size:14px\">'+tip+'</p>';"
    "      setTimeout(function(){loadScan(retries+1);},2000);"
    "    } else {"
    "      renderAps(list);"
    "    }"
    "  }).catch(function(){"
    "    document.getElementById('aps').innerHTML='<p style=\"color:#c00;font-size:14px\">加载失败，请手动刷新</p>';"
    "  });"
    "}"

    /* 渲染 AP 列表 */
    "function renderAps(list){"
    "  var el=document.getElementById('aps');"
    "  if(!list||!list.length){el.innerHTML='<p style=\"color:#999;font-size:14px\">未找到网络</p>';return;}"
    "  el.innerHTML=list.map(function(ap){"
    "    var lk=ap.auth?'&#x1F512;':'&#x1F513;';"
    "    return '<a class=\"ap-item\" href=\"#\" onclick=\"pick(\\''+ap.ssid+'\\');return false\">'"
    "      +ap.ssid+' ('+ap.rssi+' dBm) '+lk+'</a>';"
    "  }).join('');"
    "}"

    /* 手动重新扫描 */
    "function doRescan(){"
    "  var sm=document.getElementById('smsg');"
    "  var rb=document.getElementById('rbtn');"
    "  sm.className='msg info';sm.style.display='block';"
    "  sm.innerText='\u6b63\u5728\u91cd\u65b0\u626b\u63cf\uff0c\u82e5\u65ad\u7ebf\u8bf7\u91cd\u65b0\u8fde\u63a5\u70ed\u70b9...';"
    "  rb.disabled=true;"
    "  document.getElementById('aps').innerHTML='<p style=\"color:#999;font-size:14px\">扫描中...</p>';"
    "  fetch('/rescan').then(function(){"
    "    setTimeout(function(){"
    "      fetch('/scan').then(function(r){return r.json()}).then(function(list){"
    "        renderAps(list);sm.style.display='none';rb.disabled=false;"
    "      });"
    "    },3500);"
    "  });"
    "}"

    /* 点击 AP 列表填入 SSID */
    "function pick(s){document.getElementById('ssid').value=s;document.getElementById('pwd').focus();}"

    "function doConn(){"
    "  var m=document.getElementById('wmsg');"
    "  m.className='msg info';m.style.display='block';"
    "  m.innerText='\u6b63\u5728\u8fde\u63a5\uff0c\u8bf7\u7a0d\u5019...';"
    "  fetch('/connect',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify({ssid:document.getElementById('ssid').value,"
    "      password:document.getElementById('pwd').value})"
    "  }).then(function(r){return r.json()}).then(function(d){"
    "    if(d.success){pollResult(0);}"
    "    else{m.className='msg err';m.innerText='\u8fde\u63a5\u5931\u8d25\uff1a'+(d.error||'\u672a\u77e5\u9519\u8bef');}"
    "  }).catch(function(){m.className='msg err';m.innerText='\u8bf7\u6c42\u8d85\u65f6\uff0c\u8bf7\u91cd\u8bd5';});"
    "}"
    "function pollResult(n){"
    "  var m=document.getElementById('wmsg');"
    "  if(n>20){m.className='msg ok';m.innerText='\u914d\u7f51\u5df2\u63d0\u4ea4\uff0c\u8bf7\u67e5\u770b\u8bbe\u5907\u5c4f\u5e55\u83b7\u53d6IP';return;}"
    "  setTimeout(function(){"
    "    fetch('/result').then(function(r){return r.json();}).then(function(d){"
    "      if(d.done){"
    "        if(d.success){m.className='msg ok';m.innerText='\u8fde\u63a5\u6210\u529f\uff01IP: '+d.ip;}"
    "        else{m.className='msg err';m.innerText='\u8fde\u63a5\u5931\u8d25\uff0c\u8bf7\u68c0\u67e5\u5bc6\u7801\u540e\u91cd\u8bd5';}"
    "      }else{pollResult(n+1);}"
    "    }).catch(function(){pollResult(n+1);});"
    "  },1000);"
    "}"

    /* MQTT 保存 */
    "function saveMqtt(){"
    "  var m=document.getElementById('mmsg');"
    "  m.className='msg info';m.style.display='block';m.innerText='正在保存...';"
    "  fetch('/mqtt-config',{method:'POST',"
    "    headers:{'Content-Type':'application/json'},"
    "    body:JSON.stringify({"
    "      host:document.getElementById('mhost').value,"
    "      port:parseInt(document.getElementById('mport').value)||1883,"
    "      client_id:document.getElementById('mcid').value,"
    "      username:document.getElementById('muser').value,"
    "      password:document.getElementById('mpass').value,"
    "      sub_topic:document.getElementById('msub').value,"
    "      pub_topic:document.getElementById('mpub').value"
    "    })"
    "  }).then(function(r){return r.json()}).then(function(d){"
    "    if(d.success){m.className='msg ok';m.innerText='保存成功！';}"
    "    else{m.className='msg err';m.innerText='保存失败：'+d.error;}"
    "  }).catch(function(){m.className='msg err';m.innerText='请求超时，请重试';});"
    "}"
    "</script></body></html>";

/* ──────────────────────────────────────────────────────── */
/* GET /                                                     */
/* ──────────────────────────────────────────────────────── */
static esp_err_t handler_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
    return ESP_OK;
}

/* ──────────────────────────────────────────────────────── */
/* GET /scan — 返回扫描到的 AP 列表（JSON，带缓存）                        */
/* 注：不使用 cJSON 动态内存，改用直接字符串拼接，避免堆分片导致 cJSON 失败 */
/* ──────────────────────────────────────────────────────── */
static esp_err_t handler_scan(httpd_req_t *req)
{
    if (!s_scan_cached) {
        wifi_ap_record_t *records = NULL;
        uint16_t num = 0;
        bool got = wifi_manager_get_scan_results(&records, &num);
        uart_send_eventf("DBG:scan_hdl,got=%d,num=%u,heap=%u",
                         (int)got, (unsigned)num,
                         (unsigned)esp_get_free_heap_size());

        if (got && num > 0 && records) {
            /* 直接拼接 JSON，零堆内存分配，避免 cJSON 在 Wi-Fi 占用大量堆后分配失败 */
            char *p   = s_scan_cache;
            char *end = s_scan_cache + sizeof(s_scan_cache) - 4;
            *p++ = '[';
            bool first = true;
            for (int i = 0; i < (int)num && p < end - 120; i++) {
                if (records[i].ssid[0] == '\0') continue;
                /* 项分隔符 */
                if (!first) *p++ = ',';
                first = false;
                /* 开头 */
                int w = snprintf(p, end - p, "{\"ssid\":\"");
                p += w;
                /* SSID 转义：对 '\"' '\\' '\n' '\r' '\t' 进行转义 */
                const uint8_t *s = records[i].ssid;
                while (*s && p < end - 10) {
                    if      (*s == '"')  { *p++ = '\\'; *p++ = '"';  }
                    else if (*s == '\\') { *p++ = '\\'; *p++ = '\\'; }
                    else if (*s == '\n') { *p++ = '\\'; *p++ = 'n';  }
                    else if (*s == '\r') { *p++ = '\\'; *p++ = 'r';  }
                    else if (*s == '\t') { *p++ = '\\'; *p++ = 't';  }
                    else                 { *p++ = (char)*s; }
                    s++;
                }
                /* 其余字段 */
                w = snprintf(p, end - p, "\",\"rssi\":%d,\"auth\":%d}",
                             (int)records[i].rssi,
                             records[i].authmode != WIFI_AUTH_OPEN ? 1 : 0);
                p += w;
            }
            *p++ = ']';
            *p   = '\0';
            ESP_LOGI(TAG, "扫描结果已缓存，共 %u 个 AP，JSON len=%d", num, (int)strlen(s_scan_cache));
        } else {
            snprintf(s_scan_cache, sizeof(s_scan_cache), "%s", "[]");
            ESP_LOGW(TAG, "预扫描无结果，返回空列表");
        }
        s_scan_cached = true;
    }

    httpd_resp_set_type(req, "application/json");
    /* 禁止浏览器缓存 /scan 响应：如果缓存了先前空列表，重连后仍会展示旧结果 */
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_send(req, s_scan_cache, strlen(s_scan_cache));
    return ESP_OK;
}

/* ──────────────────────────────────────────────────────── */
/* GET /rescan — 清缓存并触发后台重新扫描                     */
/* ──────────────────────────────────────────────────────── */
/* ──────────────────────────────────────────────────────── */
/* GET /result — 查询异步连接结果（浏览器每秒轮询）                      */
/* 0=pending, 1=成功, 2=失败                                   */
/* ──────────────────────────────────────────────────────── */
static esp_err_t handler_result(httpd_req_t *req)
{
    int  state = 0;
    char ip[16] = "";
    wifi_manager_get_connect_result(&state, ip, sizeof(ip));

    char resp[80];
    if (state == 1) {
        snprintf(resp, sizeof(resp),
                 "{\"done\":true,\"success\":true,\"ip\":\"%s\"}", ip);
    } else if (state == 2) {
        snprintf(resp, sizeof(resp),
                 "{\"done\":true,\"success\":false,\"ip\":\"\"}");
    } else {
        snprintf(resp, sizeof(resp), "{\"done\":false}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t handler_rescan(httpd_req_t *req)
{
    s_scan_cached = false;
    snprintf(s_scan_cache, sizeof(s_scan_cache), "%s", "[]");
    wifi_manager_trigger_rescan();
    ESP_LOGI(TAG, "手动触发重新扫描");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"scanning\":true}", 17);
    return ESP_OK;
}

/* ──────────────────────────────────────────────────────── */
/* GET /captive-done — 页面加载完毕后调用                     */
/* ──────────────────────────────────────────────────────── */
static esp_err_t handler_captive_done(httpd_req_t *req)
{
    s_portal_done = true;
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    ESP_LOGI(TAG, "Captive Portal 已完成");
    return ESP_OK;
}

/* ──────────────────────────────────────────────────────── */
/* POST /connect — 接收 SSID+密码，连接并保存              */
/* ──────────────────────────────────────────────────────── */
static esp_err_t handler_connect(httpd_req_t *req)
{
    char body[256] = {0};
    int  ret = httpd_req_recv(req, body, sizeof(body) - 1);
    if (ret <= 0) {
        httpd_resp_send_408(req);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_set_status(req, HTTPD_400);
        httpd_resp_send(req, "Bad Request", strlen("Bad Request"));
        return ESP_FAIL;
    }

    cJSON *j_ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *j_pwd  = cJSON_GetObjectItem(root, "password");

    if (!j_ssid || !cJSON_IsString(j_ssid)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, HTTPD_400);
        httpd_resp_send(req, "Missing ssid", strlen("Missing ssid"));
        return ESP_FAIL;
    }

    const char *ssid = j_ssid->valuestring;
    const char *pwd  = (j_pwd && cJSON_IsString(j_pwd)) ? j_pwd->valuestring : "";

    ESP_LOGI(TAG, "收到配网请求：SSID=%s", ssid);

    char ip_str[16] = {0};
    bool ok = wifi_manager_try_connect(ssid, pwd, ip_str, sizeof(ip_str));
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    if (ok) {
        const char *resp = "{\"success\":true,\"pending\":true,\"ip\":\"\",\"msg\":\"Request accepted. AP will close in 2s and start connecting.\"}";
        httpd_resp_send(req, resp, strlen(resp));
    } else {
        const char *err_msg = "{\"success\":false,\"error\":\"request rejected\"}";
        httpd_resp_send(req, err_msg, strlen(err_msg));
    }
    return ESP_OK;
}

/* ──────────────────────────────────────────────────────── */
/* GET /mqtt-config — 读取 NVS 中保存的 MQTT 配置           */
/* ──────────────────────────────────────────────────────── */
static esp_err_t handler_mqtt_get(httpd_req_t *req)
{
    nvs_handle_t h;
    cJSON *obj = cJSON_CreateObject();

    if (nvs_open(NVS_NS_MQTT, NVS_READONLY, &h) == ESP_OK) {
        char   buf[128];
        size_t len;
        uint16_t port = 1883;

        len = sizeof(buf); if (nvs_get_str(h, "host",      buf, &len) == ESP_OK) cJSON_AddStringToObject(obj, "host",      buf);
        if (nvs_get_u16(h, "port", &port) == ESP_OK)                              cJSON_AddNumberToObject(obj, "port",      port);
        len = sizeof(buf); if (nvs_get_str(h, "client_id", buf, &len) == ESP_OK) cJSON_AddStringToObject(obj, "client_id", buf);
        len = sizeof(buf); if (nvs_get_str(h, "username",  buf, &len) == ESP_OK) cJSON_AddStringToObject(obj, "username",  buf);
        len = sizeof(buf); if (nvs_get_str(h, "sub_topic", buf, &len) == ESP_OK) cJSON_AddStringToObject(obj, "sub_topic", buf);
        len = sizeof(buf); if (nvs_get_str(h, "pub_topic", buf, &len) == ESP_OK) cJSON_AddStringToObject(obj, "pub_topic", buf);
        nvs_close(h);
    }

    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    return ESP_OK;
}

/* ──────────────────────────────────────────────────────── */
/* POST /mqtt-config — 保存 MQTT 配置到 NVS，并通知 STM32   */
/* ──────────────────────────────────────────────────────── */
static esp_err_t handler_mqtt_post(httpd_req_t *req)
{
    char body[384] = {0};
    int  ret = httpd_req_recv(req, body, sizeof(body) - 1);
    if (ret <= 0) {
        httpd_resp_send_408(req);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_set_status(req, HTTPD_400);
        httpd_resp_send(req, "Bad Request", strlen("Bad Request"));
        return ESP_FAIL;
    }

    cJSON *j_host   = cJSON_GetObjectItem(root, "host");
    cJSON *j_port   = cJSON_GetObjectItem(root, "port");
    cJSON *j_cid    = cJSON_GetObjectItem(root, "client_id");
    cJSON *j_user   = cJSON_GetObjectItem(root, "username");
    cJSON *j_pass   = cJSON_GetObjectItem(root, "password");
    cJSON *j_sub    = cJSON_GetObjectItem(root, "sub_topic");
    cJSON *j_pub    = cJSON_GetObjectItem(root, "pub_topic");

    if (!j_host || !cJSON_IsString(j_host) || strlen(j_host->valuestring) == 0) {
        cJSON_Delete(root);
        const char *e = "{\"success\":false,\"error\":\"host empty\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, e, strlen(e));
        return ESP_OK;
    }

    const char *host    = j_host->valuestring;
    uint16_t    port    = (j_port && cJSON_IsNumber(j_port)) ? (uint16_t)j_port->valuedouble : 1883;
    const char *cid     = (j_cid  && cJSON_IsString(j_cid))  ? j_cid->valuestring  : "";
    const char *user    = (j_user && cJSON_IsString(j_user)) ? j_user->valuestring  : "";
    const char *pass    = (j_pass && cJSON_IsString(j_pass)) ? j_pass->valuestring  : "";
    const char *sub     = (j_sub  && cJSON_IsString(j_sub))  ? j_sub->valuestring   : "";
    const char *pub     = (j_pub  && cJSON_IsString(j_pub))  ? j_pub->valuestring   : "";

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_MQTT, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_set_str(h, "host",      host);
        nvs_set_u16(h, "port",      port);
        nvs_set_str(h, "client_id", cid);
        nvs_set_str(h, "username",  user);
        nvs_set_str(h, "password",  pass);
        nvs_set_str(h, "sub_topic", sub);
        nvs_set_str(h, "pub_topic", pub);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "MQTT 配置已保存：%s:%u  sub=%s  pub=%s", host, port, sub, pub);
    }

    uart_send_eventf("MQTT_CFG,%s,%u,%s,%s,%s,%s", host, (unsigned)port, cid, user, sub, pub);

    cJSON_Delete(root);

    const char *resp = (err == ESP_OK)
        ? "{\"success\":true}"
        : "{\"success\":false,\"error\":\"nvs write failed\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

/* ──────────────────────────────────────────────────────── */
/* Captive Portal 重定向（各平台检测端点）                   */
/* ──────────────────────────────────────────────────────── */
static esp_err_t handler_redirect(httpd_req_t *req)
{
    if (s_portal_done) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    static const char REDIR_HTML[] =
        "<html><head>"
        "<meta http-equiv='refresh' content='0;url=http://192.168.4.1/'>"
        "</head><body>"
        "<a href='http://192.168.4.1/'>Click here to configure Wi-Fi</a>"
        "</body></html>";
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, REDIR_HTML, strlen(REDIR_HTML));
    return ESP_OK;
}

static esp_err_t handler_catch_all(httpd_req_t *req)
{
    return handler_redirect(req);
}

/* ──────────────────────────────────────────────────────── */
/* 公共接口                                                  */
/* ──────────────────────────────────────────────────────── */
esp_err_t http_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 20;
    cfg.stack_size       = 12288;
    cfg.lru_purge_enable = false;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务器启动失败");
        return ESP_FAIL;
    }

    httpd_uri_t routes[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = handler_index    },
        { .uri = "/scan",        .method = HTTP_GET,  .handler = handler_scan     },
        { .uri = "/connect",     .method = HTTP_POST, .handler = handler_connect  },
        { .uri = "/result",      .method = HTTP_GET,  .handler = handler_result   },
        { .uri = "/mqtt-config", .method = HTTP_GET,  .handler = handler_mqtt_get  },
        { .uri = "/mqtt-config", .method = HTTP_POST, .handler = handler_mqtt_post },
        { .uri = "/rescan",          .method = HTTP_GET,  .handler = handler_rescan       },
        { .uri = "/captive-done",    .method = HTTP_GET,  .handler = handler_captive_done },
        { .uri = "/generate_204",           .method = HTTP_GET, .handler = handler_redirect },
        { .uri = "/hotspot-detect.html",    .method = HTTP_GET, .handler = handler_redirect },
        { .uri = "/ncsi.txt",               .method = HTTP_GET, .handler = handler_redirect },
        { .uri = "/connectivity-check.html",.method = HTTP_GET, .handler = handler_redirect },
        { .uri = "/connecttest.txt",        .method = HTTP_GET, .handler = handler_redirect },
        { .uri = "/success.txt",            .method = HTTP_GET, .handler = handler_redirect },
        { .uri = "/redirect",               .method = HTTP_GET, .handler = handler_redirect },
        { .uri = "/*",                      .method = HTTP_GET, .handler = handler_catch_all },
    };

    for (int i = 0; i < (int)(sizeof(routes) / sizeof(routes[0])); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }

    ESP_LOGI(TAG, "HTTP 服务器已启动：http://192.168.4.1");
    return ESP_OK;
}

void http_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "HTTP 服务器已停止");
    }
    s_portal_done = false;
    s_scan_cached  = false;
    snprintf(s_scan_cache, sizeof(s_scan_cache), "%s", "[]");
}

void http_server_clear_scan_cache(void)
{
    s_scan_cached = false;
    snprintf(s_scan_cache, sizeof(s_scan_cache), "%s", "[]");
    ESP_LOGI(TAG, "HTTP 扫描缓存已清除");
}
