# Waveshare ESP32-S3-Touch-AMOLED-2.06


[ESP32-S3-Touch-AMOLED-2.06](https://www.waveshare.com/esp32-s3-touch-amoled-2.06.htm) is a high-performance, wearable watch-style development board designed by Waveshare. Based on the ESP32-S3R8 microcontroller, it integrates a 2.06inch AMOLED capacitive touch display, 6-axis IMU, RTC chip, audio codec chip, power management IC, and so on. Comes with a custom-designed case with a smartwatch-like appearance, making it ideal for prototyping and functional verification of wearable applications.

## images

| [ESP32-S3-Touch-AMOLED-2.06](https://www.waveshare.com/esp32-s3-touch-amoled-2.06.htm)                                                                                                                                       |
|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| <img style="width: 150px; height: auto; display: block; margin: 0 auto;" src= "https://www.waveshare.com/media/catalog/product/cache/1/image/800x800/9df78eab33525d08d6e5fb8d27136e95/e/s/esp32-s3-touch-amoled-2.06-1.jpg"> |

## Build

Use the board release script for this board instead of switching board settings
manually through `sdkconfig`:

```bash
source ~/.espressif/tools/activate_idf_v5.5.4.sh
cd firmware/note-watch-206
python scripts/release.py waveshare/esp32-s3-touch-amoled-2.06
```

This applies the board-specific Kconfig options from `config.json`, including
the S3 board type, 32MB flash size, and the 32MB partition table.

Avoid using a stale `build/` directory after switching boards. A stale build
cache can keep the previous board type or flash size, such as `16MB`, even after
`sdkconfig` was edited.
