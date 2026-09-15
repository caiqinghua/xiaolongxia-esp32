#pragma once
#include <cstddef>
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr int ESP_TLS_ERR_SSL_WANT_READ = -10;
constexpr int ESP_TLS_ERR_SSL_WANT_WRITE = -11;
struct esp_tls_t { int fd = 7; };
using esp_tls_error_handle_t = void*;
struct esp_tls_cfg_t { void (*crt_bundle_attach)() = nullptr; int timeout_ms = 0; };
esp_tls_t* esp_tls_init();
int esp_tls_conn_new_sync(const char*, size_t, int, const esp_tls_cfg_t*, esp_tls_t*);
esp_err_t esp_tls_get_error_handle(esp_tls_t*, esp_tls_error_handle_t*);
esp_err_t esp_tls_get_and_clear_last_error(esp_tls_error_handle_t, int*, int*);
esp_err_t esp_tls_get_conn_sockfd(esp_tls_t*, int*);
int esp_tls_conn_destroy(esp_tls_t*);
int esp_tls_conn_read(esp_tls_t*, void*, size_t);
int esp_tls_conn_write(esp_tls_t*, const void*, size_t);
