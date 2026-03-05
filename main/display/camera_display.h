#ifndef _CAMERA_DISPLAY_H_
#define _CAMERA_DISPLAY_H_

#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class CameraDisplay {
public:
    CameraDisplay();
    ~CameraDisplay();

    bool Start();
    void Stop();
    bool IsRunning() const { return is_running_; }

    // 设置相机图像控件
    void SetCameraImage(lv_obj_t* img) { camera_image_ = img; }

private:
    static void CameraTask(void* arg);

    lv_obj_t* camera_image_;
    TaskHandle_t camera_task_handle_;
    bool is_running_;
};

#endif // _CAMERA_DISPLAY_H_
