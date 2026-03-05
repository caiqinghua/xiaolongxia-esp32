#include "camera_display.h"
#include "board.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

#define TAG "CameraDisplay"

CameraDisplay::CameraDisplay()
    : camera_image_(nullptr)
    , camera_task_handle_(nullptr)
    , is_running_(false) {
}

CameraDisplay::~CameraDisplay() {
    Stop();
}

bool CameraDisplay::Start() {
    if (is_running_) {
        ESP_LOGW(TAG, "Camera already running");
        return true;
    }

    ESP_LOGI(TAG, "Starting camera display (using esp_camera)");

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == NULL) {
        ESP_LOGE(TAG, "esp_camera not initialized");
        return false;
    }

    ESP_LOGI(TAG, "Camera sensor found, starting display task");

    is_running_ = true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        CameraTask,
        "camera_task",
        8 * 1024,
        this,
        5,
        &camera_task_handle_,
        1
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create camera task");
        is_running_ = false;
        return false;
    }

    return true;
}

void CameraDisplay::Stop() {
    if (!is_running_) {
        return;
    }

    ESP_LOGI(TAG, "Stopping camera display");
    is_running_ = false;

    if (camera_task_handle_) {
        for (int i = 0; i < 20 && camera_task_handle_ != nullptr; ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (camera_task_handle_ != nullptr) {
            vTaskDelete(camera_task_handle_);
            camera_task_handle_ = nullptr;
        }
    }

    if (camera_image_ != nullptr) {
        if (lvgl_port_lock(0)) {
            lv_obj_del(camera_image_);
            camera_image_ = nullptr;
            lvgl_port_unlock();
        }
    }
}

void CameraDisplay::CameraTask(void* arg) {
    CameraDisplay* display = static_cast<CameraDisplay*>(arg);

    ESP_LOGI(TAG, "Camera task started");

    vTaskDelay(pdMS_TO_TICKS(500));

    int frame_count = 0;
    int error_count = 0;

    // 分配 RGB565 缓冲区（用于字节交换）
    uint16_t *rgb_buffer = (uint16_t*)malloc(640 * 480 * 2);
    if (rgb_buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate RGB buffer");
        display->camera_task_handle_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    while (display->is_running_) {
        camera_fb_t *fb = esp_camera_fb_get();

        if (fb != NULL) {
            if (frame_count % 30 == 0) {
                ESP_LOGI(TAG, "[Frame %d] %dx%d, len=%d, format=%d",
                         frame_count, fb->width, fb->height, fb->len, fb->format);
            }

            // 相机输出的 RGB565 是大端格式，需要交换字节为小端
            const uint8_t *src = fb->buf;
            size_t pixel_count = fb->width * fb->height;
            
            for (size_t i = 0; i < pixel_count; i++) {
                // 交换字节：大端 -> 小端
                rgb_buffer[i] = ((uint16_t)src[i * 2] << 8) | (uint16_t)src[i * 2 + 1];
            }

            if (frame_count == 0) {
                ESP_LOGI(TAG, "First frame: buf=%p, len=%d, w=%d, h=%d", fb->buf, fb->len, fb->width, fb->height);
                ESP_LOGI(TAG, "First 4 bytes: %02X %02X %02X %02X", fb->buf[0], fb->buf[1], fb->buf[2], fb->buf[3]);
                ESP_LOGI(TAG, "Swapped pixels[0-2]: 0x%04X 0x%04X 0x%04X", rgb_buffer[0], rgb_buffer[1], rgb_buffer[2]);
            }
            
            if (lvgl_port_lock(0)) {
                // 检查图像控件是否存在
                if (display->camera_image_ != nullptr) {
                    lv_image_dsc_t img_dsc;
                    img_dsc.header.w = fb->width;
                    img_dsc.header.h = fb->height;
                    img_dsc.data_size = fb->width * fb->height * 2;
                    img_dsc.data = (const uint8_t*)rgb_buffer;
                    img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;

                    if (frame_count == 0) {
                        ESP_LOGI(TAG, "Setting image: %dx%d, cf=%d", fb->width, fb->height, img_dsc.header.cf);
                    }
                    lv_image_set_src(display->camera_image_, &img_dsc);
                    lv_obj_set_size(display->camera_image_, fb->width, fb->height);
                    // 设置相机图像旋转 270 度（逆时针 90 度）来抵消 LVGL 的 90 度旋转
                    lv_image_set_rotation(display->camera_image_, 2700);  // 2700 = 270.0 度 (LVGL 使用 10 倍精度)
                    lv_obj_invalidate(display->camera_image_);
                } else {
                    ESP_LOGE(TAG, "camera_image_ is NULL!");
                }
                lvgl_port_unlock();
            }

            esp_camera_fb_return(fb);
            frame_count++;
            error_count = 0;
        } else {
            ESP_LOGW(TAG, "Failed to get camera frame");
            error_count++;
        }

        if (error_count > 10) {
            ESP_LOGE(TAG, "Too many errors, waiting 1 second...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            error_count = 0;
        } else {
            vTaskDelay(pdMS_TO_TICKS(33));
        }
    }

    ESP_LOGI(TAG, "Camera task stopped, captured %d frames", frame_count);
    free(rgb_buffer);
    vTaskDelete(nullptr);
}
