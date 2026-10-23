// 实现配网页的 ROM 上传通路：raw body 直传，不做 multipart 解析。
//
//   GET  /roms/list                  → text/plain，每行 "name<TAB>size"
//   POST /roms/upload?name=xxx.gb    → application/octet-stream 原始字节
//   POST /roms/delete?name=xxx.gb    → 删除 ROM 与同名 .sav
//
// ── 已知限制：URL query 不做百分号解码（2026-10-21 决策，暂不修）──
// esp_http_server 的 httpd_query_key_value() 只 memcpy 原始字节，不解码
// （IDF v5.5.3 esp_http_server.h:1019 原文 "The components of URL query
// string (keys and values) are not URLdecoded"；实现在 httpd_parse.c:931）。
// 前端 wifi_portal_pages.cpp:67/74 发请求时用了 encodeURIComponent()，
// 于是含中文/空格/&/+ 的名字到达本文件时仍是 %XX 形态。各路径实际表现：
//   上传：安全。portal 的 ok() 正则（wifi_portal_pages.cpp:55）已把文件名
//         限制在 [A-Za-z0-9._-]，这个字符集经 encodeURIComponent 原样透传，
//         非 ASCII 名在浏览器端就被拦下，有明确提示文案。
//   删除：受限。/roms/list 反映 TF 卡真实文件名，含中文名的 ROM（从电脑
//         拔卡拷贝进去的）点「删除」会发出 %XX 名字，服务端 stat 不到，
//         返回 404「ROM 不存在」。中文名 ROM 只能拔卡在电脑上删除。
//   伪造请求：name_chars_allowed() 是黑名单、放行 '%'，手工构造
//         ?name=%E4%B8%AD.gb 的上传会静默写出一个叫 "%E4%B8%AD.gb"
//         的垃圾文件（失败模式从响亮变安静）。仅限 curl 之类手工请求，
//         portal 页面走不到这一步。
// 要修的话：在 extract_rom_name() 里对 httpd_query_key_value 的结果做一次
// 百分号解码（'%XX' → 字节），再进 rom_store_name_is_valid()。修复前
// 请连带评估：解码后 '%' 本身应从黑名单挪回必拒项，否则 %2500 这类双重
// 编码仍能穿透。
#include "rom_upload.h"

#include "app_metadata.h"
#include "game_config.h"
#include "gameboy_rom_store.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <esp_attr.h>
#include <esp_log.h>

namespace {
constexpr const char *kRomUriList = "/roms/list";
constexpr const char *kRomUriUpload = "/roms/upload";
constexpr const char *kRomUriDelete = "/roms/delete";
constexpr size_t kQueryBufferBytes = 192;
constexpr size_t kNameBufferBytes = game_config::kMaxRomFileNameBytes;
// 路径/列表缓冲跟着 kMaxRomFileNameBytes（96，UTF-8 中文名最坏 3 字节/字）放宽：
// "/sdcard/rom/gb/"(15) + 名字(95) + ".part"(5) + '\0' = 116，原来 96 会被
// snprintf 截成一条不存在的路径，上传直接失败且没有任何提示。
constexpr size_t kPathBytes = 176;
constexpr size_t kListLineBytes = 176;
constexpr size_t kReceiveChunkBytes = 4096;
constexpr const char *kPlainTextType = "text/plain; charset=utf-8";
constexpr const char *kStatusOk = "200 OK";
constexpr const char *kStatusBadRequest = "400 Bad Request";
constexpr const char *kStatusNotFound = "404 Not Found";
constexpr const char *kStatusTooLarge = "413 Payload Too Large";
constexpr const char *kStatusServerError = "500 Internal Server Error";

// 上传缓冲放 PSRAM，避免占用 HTTP 服务器任务的栈或内部 RAM。
EXT_RAM_BSS_ATTR char s_receive_buffer[kReceiveChunkBytes];

void build_rom_path(const char *name, const char *suffix, char *out, size_t out_bytes)
{
    if (!out || out_bytes == 0) {
        return;
    }
    snprintf(out,
             out_bytes,
             "%s/%s%s",
             game_config::kRomsMountPoint,
             name ? name : "",
             suffix ? suffix : "");
}

// 从 URL query 里取 name 参数并做文件名黑名单校验。
// 注意：httpd_query_key_value() 不做百分号解码（见文件头「已知限制」），
// 前端 encodeURIComponent() 发来的中文名到这里还是 %XX 原样，非 ASCII 名
// 的删除会以 404 告终，这是已知且已文档化的限制，不是漏写了校验。
bool extract_rom_name(httpd_req_t *req, char *out, size_t out_bytes)
{
    if (!req || !out || out_bytes == 0) {
        return false;
    }
    const size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0 || query_len >= kQueryBufferBytes) {
        return false;
    }
    char query[kQueryBufferBytes] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    char name[kNameBufferBytes] = {};
    if (httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
        return false;
    }
    if (!rom_store_name_is_valid(name)) {
        return false;
    }
    snprintf(out, out_bytes, "%s", name);
    return true;
}

esp_err_t send_text(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, kPlainTextType);
    return httpd_resp_sendstr(req, body ? body : "");
}

esp_err_t list_get_handler(httpd_req_t *req)
{
    if (!rom_store_mount()) {
        return send_text(req, kStatusServerError, rom_store_diagnostic());
    }
    (void)rom_store_refresh();
    httpd_resp_set_status(req, kStatusOk);
    httpd_resp_set_type(req, kPlainTextType);
    const int count = rom_store_count();
    for (int index = 0; index < count; ++index) {
        char line[kListLineBytes] = {};
        const int written = snprintf(line,
                                     sizeof(line),
                                     "%s\t%u\n",
                                     rom_store_name(index),
                                     (unsigned)rom_store_size(index));
        if (written <= 0) {
            continue;
        }
        if (httpd_resp_send_chunk(req, line, written) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t upload_post_handler(httpd_req_t *req)
{
    char name[kNameBufferBytes] = {};
    if (!extract_rom_name(req, name, sizeof(name))) {
        return send_text(req, kStatusBadRequest, "文件名非法");
    }
    if (req->content_len <= 0 ||
        (size_t)req->content_len > game_config::kMaxUploadBytes) {
        return send_text(req, kStatusTooLarge, "ROM 超过 1.3MB 上限");
    }
    if (!rom_store_mount()) {
        return send_text(req, kStatusServerError, rom_store_diagnostic());
    }

    char path[kPathBytes] = {};
    build_rom_path(name, nullptr, path, sizeof(path));

    // 先写临时文件再 rename：
    // 直接 fopen(path,"wb") 会立刻把同名旧 ROM 截断，万一中途断网/断电，
    // 旧 ROM 已经被销毁、新的又是半截，用户一次失败的重传就把两个都丢了。
    // 写 .part 则在 rename 成功之前旧文件一直完好（rename 在 FAT 上是原子的）。
    char temp_path[kPathBytes] = {};
    build_rom_path(name, ".part", temp_path, sizeof(temp_path));

    FILE *file = fopen(temp_path, "wb");
    if (!file) {
        return send_text(req, kStatusServerError, "ROM 写入失败");
    }

    size_t received_total = 0;
    bool failed = false;
    while (received_total < (size_t)req->content_len) {
        const int chunk = httpd_req_recv(req, s_receive_buffer, kReceiveChunkBytes);
        if (chunk == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (chunk <= 0) {
            failed = true;
            break;
        }
        if (fwrite(s_receive_buffer, 1, (size_t)chunk, file) != (size_t)chunk) {
            failed = true;
            break;
        }
        received_total += (size_t)chunk;
    }
    const int flush_error = fflush(file);
    fclose(file);

    if (failed || flush_error != 0 || received_total != (size_t)req->content_len) {
        // 只删半成品 .part，同名旧 ROM 原封不动。
        (void)unlink(temp_path);
        return send_text(req, kStatusServerError, "上传中断，已删除半成品");
    }

    // 上传完整才替换旧文件；失败时旧 ROM 仍在，只是这次没更新。
    if (rename(temp_path, path) != 0) {
        (void)unlink(temp_path);
        return send_text(req, kStatusServerError, "ROM 落盘失败");
    }

    (void)rom_store_refresh();
    // 必须按名字选中：列表是 sort_names() 按文件名排序的，"最后一个"不是刚上传的那个。
    (void)rom_store_select_by_name(name);
    ESP_LOGI(TAG, "rom uploaded name=%s bytes=%u", name, (unsigned)received_total);
    return send_text(req, kStatusOk, "OK");
}

esp_err_t delete_post_handler(httpd_req_t *req)
{
    char name[kNameBufferBytes] = {};
    if (!extract_rom_name(req, name, sizeof(name))) {
        return send_text(req, kStatusBadRequest, "文件名非法");
    }
    if (!rom_store_mount()) {
        return send_text(req, kStatusServerError, rom_store_diagnostic());
    }

    char path[kPathBytes] = {};
    build_rom_path(name, nullptr, path, sizeof(path));
    struct stat info = {};
    if (stat(path, &info) != 0) {
        return send_text(req, kStatusNotFound, "ROM 不存在");
    }
    if (unlink(path) != 0) {
        return send_text(req, kStatusServerError, "ROM 删除失败");
    }

    char save_path[kPathBytes] = {};
    build_rom_path(name, game_config::kSaveFileSuffix, save_path, sizeof(save_path));
    (void)unlink(save_path);

    (void)rom_store_refresh();
    ESP_LOGI(TAG, "rom deleted name=%s", name);
    return send_text(req, kStatusOk, "OK");
}

esp_err_t register_route(httpd_handle_t server, const char *uri, httpd_method_t method,
                         esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t route = {};
    route.uri = uri;
    route.method = method;
    route.handler = handler;
    return httpd_register_uri_handler(server, &route);
}
} // namespace

esp_err_t rom_upload_register_routes(httpd_handle_t server)
{
    if (!server) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = register_route(server, kRomUriList, HTTP_GET, list_get_handler);
    if (err == ESP_OK) {
        err = register_route(server, kRomUriUpload, HTTP_POST, upload_post_handler);
    }
    if (err == ESP_OK) {
        err = register_route(server, kRomUriDelete, HTTP_POST, delete_post_handler);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rom routes register failed: %s", esp_err_to_name(err));
    }
    return err;
}
