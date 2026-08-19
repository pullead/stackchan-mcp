/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
/*
 * 局域网 MCP 通道。
 *
 * 官方固件已经把一整套机器人工具注册进了 McpServer 单例，但那些工具原本只对
 * 小智云开放。这里给同一个 McpServer 再接一条传输通道：在设备上开一个 HTTP
 * 端点收 JSON-RPC，喂给 McpServer，再把回复取回来。小智云那条链路完全不受影响。
 *
 * 回复取回的办法见 patches/xiaozhi-esp32.patch 里给 McpServer 加的 reply sink：
 * McpServer 的回复固定走 Application::SendMcpMessage() 发往云端，sink 让我们能
 * 在发出去之前把本地请求的回复截下来。靠 id 区间区分两者。
 */
#include "hal.h"
#include <mooncake_log.h>
#include <mcp_server.h>
#include <esp_http_server.h>
#include <cJSON.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_netif.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>

static const std::string_view _tag = "HAL-MCP-LOCAL";

namespace {

// 本地请求的 id 保留区间。云端的 id 从小往上走，这里从 0x40000000 起，
// sink 靠这个阈值判断一条回复该交给谁。
constexpr int    kLocalIdBase  = 0x40000000;
constexpr int    kPort         = 8080;
constexpr int    kTimeoutMs    = 10000;
constexpr size_t kMaxBodyBytes = 8192;

struct PendingRequest {
    SemaphoreHandle_t done = nullptr;
    std::string       payload;

    PendingRequest() : done(xSemaphoreCreateBinary()) {}
    ~PendingRequest()
    {
        if (done) {
            vSemaphoreDelete(done);
        }
    }
};

// 两端都持 shared_ptr，信号量的生命周期由最后一个持有者收尾，
// 避免超时侧删掉信号量时 sink 还在用。
std::mutex                                     g_mutex;
std::map<int, std::shared_ptr<PendingRequest>> g_pending;
int                                            g_next_local_id = kLocalIdBase;
httpd_handle_t                                 g_server        = nullptr;

// McpServer 回复时调用。可能跑在主线程（tools/call 走 app.Schedule）。
// 返回 true = 这条回复归本地，云端不该看到。
bool on_local_reply(int id, const std::string& payload)
{
    if (id < kLocalIdBase) {
        return false;  // 云端的请求，照原路发走
    }

    std::shared_ptr<PendingRequest> req;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto                        it = g_pending.find(id);
        if (it == g_pending.end()) {
            // 已经超时注销了。仍然认领掉，否则这条本地回复会被误发给云端。
            return true;
        }
        req = it->second;
        g_pending.erase(it);
    }

    req->payload = payload;
    xSemaphoreGive(req->done);
    return true;
}

bool token_ok(httpd_req_t* req)
{
    const char* expected = CONFIG_LOCAL_MCP_TOKEN;
    if (expected == nullptr || expected[0] == '\0') {
        return true;  // 未配置 token 即不鉴权
    }

    size_t len = httpd_req_get_hdr_value_len(req, "X-Auth-Token");
    if (len == 0 || len > 128) {
        return false;
    }
    char got[129] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Auth-Token", got, sizeof(got)) != ESP_OK) {
        return false;
    }
    return strcmp(got, expected) == 0;
}

esp_err_t mcp_post_handler(httpd_req_t* req)
{
    if (!token_ok(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "bad token");
        return ESP_FAIL;
    }

    if (req->content_len == 0 || req->content_len > kMaxBodyBytes) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body size");
        return ESP_FAIL;
    }

    std::string body(req->content_len, '\0');
    size_t      received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, &body[received], req->content_len - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_FAIL;
        }
        received += r;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }

    cJSON* id_item = cJSON_GetObjectItem(root, "id");

    // 通知（没有 id）：转进去就完事，没有回复可等。
    if (id_item == nullptr) {
        McpServer::GetInstance().ParseMessage(root);
        cJSON_Delete(root);
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, nullptr, 0);
        return ESP_OK;
    }

    // MCP 允许 id 是字符串或数字，而 McpServer 只认 int。
    // 这里把原始 id 原样留着，回复时再换回去。
    char* orig_id = cJSON_PrintUnformatted(id_item);

    auto pending = std::make_shared<PendingRequest>();
    if (pending->done == nullptr) {
        cJSON_free(orig_id);
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no semaphore");
        return ESP_FAIL;
    }

    int local_id;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        local_id = g_next_local_id++;
        if (g_next_local_id < kLocalIdBase) {
            g_next_local_id = kLocalIdBase;  // 溢出回绕
        }
        g_pending[local_id] = pending;
    }

    cJSON_ReplaceItemInObject(root, "id", cJSON_CreateNumber(local_id));
    McpServer::GetInstance().ParseMessage(root);
    cJSON_Delete(root);

    bool ok = xSemaphoreTake(pending->done, pdMS_TO_TICKS(kTimeoutMs)) == pdTRUE;
    if (!ok) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pending.erase(local_id);
        cJSON_free(orig_id);
        mclog::tagWarn(_tag, "request {} timed out", local_id);
        httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "device timeout");
        return ESP_FAIL;
    }

    // 把 id 换回客户端原本用的那个
    std::string out = pending->payload;
    cJSON*      resp = cJSON_Parse(out.c_str());
    if (resp != nullptr) {
        cJSON* restored = cJSON_Parse(orig_id);
        if (restored != nullptr) {
            cJSON_ReplaceItemInObject(resp, "id", restored);
        }
        char* printed = cJSON_PrintUnformatted(resp);
        if (printed != nullptr) {
            out = printed;
            cJSON_free(printed);
        }
        cJSON_Delete(resp);
    }
    cJSON_free(orig_id);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out.c_str(), out.size());
    return ESP_OK;
}

}  // namespace

namespace {

void start_http_server()
{
    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.server_port      = kPort;
    config.ctrl_port        = kPort + 1;  // 避开小智配网门户占用的默认控制端口
    config.lru_purge_enable = true;
    config.stack_size       = 8192;

    esp_err_t err = httpd_start(&g_server, &config);
    if (err != ESP_OK) {
        mclog::tagError(_tag, "httpd_start failed: {}", esp_err_to_name(err));
        g_server = nullptr;
        return;
    }

    static const httpd_uri_t mcp_uri = {
        .uri      = "/mcp",
        .method   = HTTP_POST,
        .handler  = mcp_post_handler,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(g_server, &mcp_uri);
}

// httpd_start() 会经由 LwIP 的 tcpip 线程发消息，而那个线程要到 esp_netif_init()
// 之后才存在。本函数在 main.cpp 中于 startXiaozhi()（网络在其内部才起来）之前被调用，
// 那时直接 httpd_start 会触发
//   assert failed: tcpip_send_msg_wait_sem ... (Invalid mbox)
// 并进入启动循环。所以这里起一个小任务，等拿到 IP 再启动服务。
void mcp_local_wait_task(void*)
{
    esp_netif_ip_info_t ip_info = {};

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif == nullptr) {
            continue;
        }
        if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
            continue;
        }
        if (ip_info.ip.addr == 0) {
            continue;
        }
        break;
    }

    start_http_server();

    if (g_server != nullptr) {
        mclog::tagInfo(_tag, "local MCP endpoint listening on POST http://{}.{}.{}.{}:{}/mcp",
                       esp_ip4_addr1_16(&ip_info.ip), esp_ip4_addr2_16(&ip_info.ip),
                       esp_ip4_addr3_16(&ip_info.ip), esp_ip4_addr4_16(&ip_info.ip), kPort);
    }

    vTaskDelete(nullptr);
}

}  // namespace

void Hal::mcp_local_server_init()
{
    if (g_server != nullptr) {
        mclog::tagInfo(_tag, "already started");
        return;
    }

    McpServer::GetInstance().SetLocalReplySink(on_local_reply);

    // 不能在这里直接 httpd_start，网络栈还没起来，见 mcp_local_wait_task 的注释。
    mclog::tagInfo(_tag, "waiting for network before starting local MCP endpoint");
    xTaskCreatePinnedToCore(mcp_local_wait_task, "mcp_local", 4096, nullptr, 3, nullptr, 0);
}
