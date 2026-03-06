#include "camera_display.h"
#include "board.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>
#include "jpeg_to_image.h"

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

    ESP_LOGI(TAG, "Starting camera display (using esp_camera + JPEG decode)");

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == NULL) {
        ESP_LOGE(TAG, "esp_camera not initialized");
        return false;
    }

    ESP_LOGI(TAG, "Camera sensor found, starting display task");
    ESP_LOGI(TAG, "Using JPEG format with decode pipeline for preview");

    is_running_ = true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        CameraTask,
        "camera_task",
        16 * 1024,  // 增加栈空间用于JPEG解码
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

    ESP_LOGI(TAG, "Camera task started (JPEG decode pipeline)");

    vTaskDelay(pdMS_TO_TICKS(500));

    int frame_count = 0;
    int error_count = 0;
    int decode_error_count = 0;

    uint8_t *decoded_rgb = nullptr;  // 解码后的RGB缓冲区
    size_t decoded_size = 0;

    while (display->is_running_) {
        camera_fb_t *fb = esp_camera_fb_get();

        if (fb != NULL) {
            // 每30帧打印一次信息
            if (frame_count % 30 == 0) {
                ESP_LOGI(TAG, "[Frame %d] JPEG: %dx%d, len=%d bytes",
                         frame_count, fb->width, fb->height, fb->len);
            }

            // 检查是否是JPEG格式
            if (fb->format != PIXFORMAT_JPEG) {
                ESP_LOGW(TAG, "Unexpected format: %d, expected JPEG (4)", fb->format);
                esp_camera_fb_return(fb);
                error_count++;
                vTaskDelay(pdMS_TO_TICKS(33));
                continue;
            }

            // 验证JPEG魔数
            if (fb->len < 2 || fb->buf[0] != 0xFF || fb->buf[1] != 0xD8) {
                ESP_LOGE(TAG, "Invalid JPEG data: %02X %02X", fb->buf[0], fb->buf[1]);
                esp_camera_fb_return(fb);
                error_count++;
                vTaskDelay(pdMS_TO_TICKS(33));
                continue;
            }

            // 解码JPEG为RGB565
            size_t width = 0, height = 0, stride = 0;
            uint8_t *rgb_data = nullptr;
            size_t rgb_len = 0;

            esp_err_t ret = jpeg_to_image(fb->buf, fb->len, &rgb_data, &rgb_len, &width, &height, &stride);

            if (ret != ESP_OK || rgb_data == nullptr) {
                ESP_LOGE(TAG, "JPEG decode failed: %d", ret);
                esp_camera_fb_return(fb);
                decode_error_count++;
                error_count++;

                // 如果连续解码失败多次，等待一下
                if (decode_error_count > 5) {
                    ESP_LOGW(TAG, "Multiple decode errors, waiting 500ms");
                    vTaskDelay(pdMS_TO_TICKS(500));
                    decode_error_count = 0;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(33));
                }
                continue;
            }

            decode_error_count = 0;  // 重置解码错误计数

            // 释放旧的解码缓冲区
            if (decoded_rgb != nullptr) {
                heap_caps_free(decoded_rgb);
                decoded_rgb = nullptr;
            }

            decoded_rgb = rgb_data;
            decoded_size = rgb_len;

            if (frame_count == 0) {
                ESP_LOGI(TAG, "First decode: %dx%d, len=%d, stride=%d", width, height, rgb_len, stride);
                ESP_LOGI(TAG, "First 4 pixels: 0x%04X 0x%04X 0x%04X 0x%04X",
                         ((uint16_t*)rgb_data)[0], ((uint16_t*)rgb_data)[1],
                         ((uint16_t*)rgb_data)[2], ((uint16_t*)rgb_data)[3]);
            }

            // 显示到LVGL
            if (lvgl_port_lock(0)) {
                if (display->camera_image_ != nullptr) {
                    // 第一次设置时，更新图像控件大小
                    if (frame_count == 0) {
                        lv_obj_set_size(display->camera_image_, width, height);
                        lv_obj_align(display->camera_image_, LV_ALIGN_CENTER, 0, 0);
                        ESP_LOGI(TAG, "Updated camera image size to %dx%d", width, height);
                    }
                    lv_image_dsc_t img_dsc;
                    img_dsc.header.w = width;
                    img_dsc.header.h = height;
                    img_dsc.data_size = rgb_len;
                    img_dsc.data = rgb_data;
                    img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;

                    lv_image_set_src(display->camera_image_, &img_dsc);
                    // 设置相机图像旋转 270 度（逆时针 90 度）来抵消 LVGL 的 90 度旋转
                    lv_image_set_rotation(display->camera_image_, 2700);

                    // 设置图像居中对齐
                    // lv_image_set_align(display->camera_image_, LV_IMAGE_ALIGN_CENTER);

                    if (frame_count == 0) {
                        ESP_LOGI(TAG, "Image displayed: %dx%d, centered in 320x480 widget", width, height);
                    }

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
            vTaskDelay(pdMS_TO_TICKS(33));  // ~30fps
        }
    }

    ESP_LOGI(TAG, "Camera task stopped, captured %d frames", frame_count);

    // 清理解码缓冲区
    if (decoded_rgb != nullptr) {
        heap_caps_free(decoded_rgb);
    }

    vTaskDelete(nullptr);
}
