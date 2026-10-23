// 声明配网页 ROM 上传/列表/删除三个 HTTP 路由的注册入口。
#pragma once

#include <esp_err.h>
#include <esp_http_server.h>

#ifdef __cplusplus
extern "C" {
#endif

// 在已启动的配网 HTTP 服务器上注册 /roms/list、/roms/upload、/roms/delete。
esp_err_t rom_upload_register_routes(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
