#pragma once
struct esp_app_desc_t { char version[32]; };
inline const esp_app_desc_t* esp_app_get_description() {
    static const esp_app_desc_t description{"2.0.0"};
    return &description;
}
