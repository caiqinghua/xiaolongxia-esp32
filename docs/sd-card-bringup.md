# Audream Note Watch SD Card Bring-up 记录

本文记录 Waveshare ESP32-S3 Touch AMOLED 2.06 上 TF/SD 卡调试过程、已经确认的结论、踩坑原因和后续验证方法。目标是让设备能在本地 TF 卡上保存 Audream Watch 录音文件，并通过 BLE 控制链路提供状态查询、录音开始/停止和文件读取能力。

## 当前结论

- 目标硬件使用 Waveshare ESP32-S3 Touch AMOLED 2.06。
- TF 卡接口实际应按 SDMMC 1-bit 模式使用，而不是 SDSPI。
- 已确认有效引脚：
  - CLK: GPIO2
  - CMD: GPIO1
  - D0: GPIO3
  - CS: GPIO17 只在官方 Arduino SPI 表格里出现，实际 SD_MMC 1-bit 示例没有使用它。
- SD 卡初始化应在设备启动流程中主动执行，iOS 只读取缓存状态或主动 refresh/remount，不应由 iOS 首次命令触发真实初始化。
- FATFS 需要启用 Long File Name，否则 `/sdcard/audream/recordings` 这类长目录名会创建失败。

## Waveshare ESP32-C6 Touch AMOLED 2.06 原理图结论

### 1. Arduino 示例没有 SD 卡引脚定义

本地 Waveshare C6 2.06 Arduino 包路径：

```text
/Users/lin/ai/voicerecord/ispeak/firmware/ESP32-C6-Touch-AMOLED-2.06/examples/Arduino-v3.3.0
```

板级 `Mylibrary/pin_config.h` 只定义了 LCD 和 I2C/touch 相关引脚：

- LCD: `LCD_SDIO0=1`, `LCD_SDIO1=2`, `LCD_SDIO2=3`, `LCD_SDIO3=4`, `LCD_SCLK=0`, `LCD_CS=5`, `LCD_RESET=11`
- I2C/touch: `IIC_SDA=8`, `IIC_SCL=7`, `TP_INT=15`, `TP_RESET=10`

顶层 Arduino examples 没有板级 `SD_*`、`TF_*`、`SDMMC` 或 `SDSPI` 定义。搜索到的 SD 内容主要来自 Arduino_GFX、LVGL、SensorLib 的通用示例，不能作为这块板子的 pin map 依据。

### 2. 原理图里的 ESP32-C6 SDIO 引脚组

参考文件：

```text
/Users/lin/ai/voicerecord/ispeak/firmware/ESP32-C6-Touch-AMOLED-2.06/Schematic/ESP32-C6-Touch-AMOLED-2.06-Schematic-V1.0.pdf
```

PDF 由 Altium Designer 导出，共 3 页。通过 macOS Quick Look 渲染第一页，并解压 PDF content stream 后重建文字坐标，确认 ESP32-C6 芯片侧 SDIO 引脚组如下：

- `SDIO_CMD`: GPIO18
- `SDIO_CLK`: GPIO19
- `SDIO_DATA0`: GPIO20
- `SDIO_DATA1`: GPIO21
- `SDIO_DATA2`: GPIO22
- `SDIO_DATA3`: GPIO23

这组信号是 ESP32-C6 芯片的 SDIO 复用引脚。当前本地样机没有焊接 SD/TF 卡座，因此无法插入实际 SD 卡验证挂载成功，但原理图读取结果本身是有效的。

### 3. C6 固件接入方式

当前 `SdCardStorage` 的 SDMMC 分支使用 `SDMMC_HOST_DEFAULT()`。ESP32-C6 目标下这条路径不可用，之前把 C6 强行配置成 `AUDREAM_SD_USE_SDMMC=1` 会导致编译失败。

因此 C6 2.06 在当前固件中按 SDSPI mode 接入同一组 SDIO 线：

- `AUDREAM_SD_MOSI_GPIO=GPIO_NUM_18`，对应 SDIO CMD
- `AUDREAM_SD_CLK_GPIO=GPIO_NUM_19`，对应 SDIO CLK
- `AUDREAM_SD_MISO_GPIO=GPIO_NUM_20`，对应 SDIO DATA0
- `AUDREAM_SD_CS_GPIO=GPIO_NUM_23`，对应 SDIO DATA3，在 SPI mode 下作为 CS
- `AUDREAM_SD_SPI_HOST=SPI2_HOST`
- `AUDREAM_SD_USE_SDMMC=0`

代码位置：

- `main/CMakeLists.txt`
- `main/storage/sd_card_storage.cc`

### 4. 未焊卡座样机的预期日志

在没有焊接 SD/TF 卡座、或没有实际插卡的 C6 2.06 样机上，启动后会看到 SDSPI mount timeout：

```text
AudreamStorage: Mounting SD card in SDSPI mode on host=1 with pins cs=23 mosi=18 miso=20 clk=19
spi_bus_initialize(...): SPI bus already initialized.
AudreamStorage: SD SPI bus was already initialized
AudreamStorage: Calling esp_vfs_fat_sdspi_mount at /sdcard
vfs_fat_sdmmc: sdmmc_card_init failed (0x107).
AudreamStorage: Failed to mount SD card: ESP_ERR_TIMEOUT (263).
```

这里的 `ESP_ERR_TIMEOUT` 不说明 pin map 一定错误；在当前实物没有 SD/TF 卡座的情况下，这是无法和卡通信的预期结果。`SPI bus already initialized` 表示 SPI host 已被系统或其他初始化路径占用过，当前 storage 代码会把 `ESP_ERR_INVALID_STATE` 当作可继续状态处理。

### 5. 后续硬件验证建议

要验证 C6 2.06 SD 存储链路，需要满足以下条件：

- 样机焊接 SD/TF 卡座，或用飞线把卡座连接到上述 SDIO 引脚组。
- 卡以 FAT32 格式化。
- 首次验证优先使用小容量或已确认可被 ESP-IDF 识别的卡。
- 启动日志应从 `sdmmc_card_init failed (0x107)` 变为打印 card info，并完成 `/sdcard` mount。

## 调试过程

### 1. 最初按 SDSPI 方向接入

最开始根据官方页面的 “SPI interface” 表格，把 TF 卡按 SDSPI 处理：

- CS: GPIO17
- MOSI: GPIO1
- MISO: GPIO3
- SCLK: GPIO2

固件里通过 `esp_vfs_fat_sdspi_mount()` 挂载。这个方向出现了两个问题：

- `spi_bus_initialize()` 报 `SPI bus already initialized`。
- 后续 `sdmmc_card_init failed (0x107)`，无法识别卡。

当时判断 SPI bus 冲突可以规避，但更关键的问题是：官方文档虽然表格写了 SPI 信号名，实际示例代码不一定走 SDSPI。

### 2. BTC_TASK 栈溢出

iOS 发 BLE 自定义命令触发 SD mount 时，曾出现：

```text
***ERROR*** A stack overflow in task BTC_TASK has been detected.
```

原因是 BLE 回调所在的 Bluetooth controller task 栈很小，不适合直接执行 SD 初始化、文件系统挂载、目录扫描等重操作。

修复方向：

- BLE custom data 回调只收包和投递任务。
- 实际 Audream device control 逻辑放入独立 `audream_ctrl` task 执行。
- SD 初始化进一步从 iOS 命令路径前移到设备启动流程。

### 3. 交叉验证 Waveshare 官方示例

为了确认官方文档是否表述混乱，交叉看了两个来源：

1. Arduino 示例：
   - 路径：`/Users/lin/ai/voicerecord/waveshare/ESP32-S3-Touch-AMOLED-2.06/examples/Arduino-v3.2.0/examples/07_LVGL_SD_Test`
   - `pin_config.h` 定义：
     - `SDMMC_CLK = 2`
     - `SDMMC_CMD = 1`
     - `SDMMC_DATA = 3`
     - `SDMMC_CS = 17`
   - 实际初始化调用：
     - `SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA)`
     - `SD_MMC.begin("/sdcard", true)`
   - 这里 `true` 表示 1-bit mode，CS 没有参与实际初始化。

2. ESP-IDF BSP：
   - 通过 Waveshare 官方 ESP-IDF 示例拉取 managed component 后确认 BSP 定义：
     - `BSP_SD_CLK = GPIO_NUM_2`
     - `BSP_SD_CMD = GPIO_NUM_1`
     - `BSP_SD_D0 = GPIO_NUM_3`
   - BSP mount 代码使用：
     - `SDMMC_HOST_DEFAULT()`
     - `SDMMC_SLOT_CONFIG_DEFAULT()`
     - `slot_config.width = 1`
     - `esp_vfs_fat_sdmmc_mount()`

最终结论：这块板子的 TF 卡应走 SDMMC 1-bit，GPIO1/2/3 是 CMD/CLK/D0，不是传统 SDSPI 的 MOSI/SCLK/MISO 语义。

### 4. 切换为 SDMMC 1-bit

当前固件对 Waveshare ESP32-S3 Touch AMOLED 2.06 使用：

- `AUDREAM_SD_USE_SDMMC=1`
- `AUDREAM_SD_CLK_GPIO=GPIO_NUM_2`
- `AUDREAM_SD_MOSI_GPIO=GPIO_NUM_1`，在 SDMMC 分支里作为 CMD 使用
- `AUDREAM_SD_MISO_GPIO=GPIO_NUM_3`，在 SDMMC 分支里作为 D0 使用
- `AUDREAM_SD_CS_GPIO=GPIO_NUM_NC`

代码路径：

- `main/CMakeLists.txt`
- `main/storage/sd_card_storage.cc`

挂载方式：

```cpp
sdmmc_host_t host = SDMMC_HOST_DEFAULT();
sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
slot_config.clk = AUDREAM_SD_CLK_GPIO;
slot_config.cmd = AUDREAM_SD_MOSI_GPIO;
slot_config.d0 = AUDREAM_SD_MISO_GPIO;
slot_config.width = 1;
esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slot_config, &mount_config, &card);
```

调试中还移除了一个错误方向：不要手动把 `host.max_freq_khz` 固定成 `SDMMC_FREQ_PROBING`。ESP-IDF 会在初始化阶段自己使用 probing frequency，再切到工作频率。固定 400kHz 不是必要配置，也可能掩盖真实问题。

### 5. 启动流程中初始化 SD 卡

当前策略：

- Board 构造完成基本硬件初始化后调用 `SdCardStorage::StartInitializationTask()`。
- 初始化任务延迟约 1 秒再 probe，避免和早期外设初始化抢资源。
- probe 最多重试 3 次，每次间隔约 500ms。
- iOS 查询 storage status 时优先拿 cached status。
- iOS refresh storage status 时调用 `ProbeAndMountStatus()` 主动重新 probe/remount。

这样做的原因：

- 设备是否有本地存储是设备自身能力，不应依赖 App 首次访问。
- BLE 命令路径应尽量轻，避免栈溢出和用户感知延迟。
- iOS 可以显示明确状态：mounted、free space、remaining minutes、error code。

### 6. SD 初始化成功后的目录创建失败

SDMMC 1-bit 修复后，设备已经能识别 64G FAT32 卡：

```text
Name: SD64G
Type: SDHC
Speed: 20.00 MHz (limit: 20.00 MHz)
CSD: ver=2, sector_size=512, capacity=124735488 read_bl_len=9
SSR: bus_width=1
SD card mounted at /sdcard
```

随后失败在：

```text
Failed to create directory /sdcard/audream/recordings: Invalid argument
```

根因是当前 FATFS 配置禁用了 Long File Name：

```text
CONFIG_FATFS_LFN_NONE=y
```

在这种配置下，FATFS 只可靠支持 8.3 文件名。`recordings` 是 10 个字符，会触发 `EINVAL`。后续 `*.opus` 和 `*.json` 文件名也会遇到同类问题。

修复方式：

- 在 `sdkconfig.defaults.esp32s3` 启用：
  - `CONFIG_FATFS_LFN_HEAP=y`
  - `CONFIG_FATFS_MAX_LFN=255`
- 本地当前 `sdkconfig` 也同步改成相同设置，方便当前 build 目录直接继续编译。

选择 heap-based LFN 的原因：

- SD 操作可能发生在不同 task 中。
- `CONFIG_FATFS_LFN_STACK` 会增加调用栈压力。
- heap-based LFN 更适合当前 BLE/control/audio task 混合调用场景。

## 当前代码状态

核心文件：

- `main/storage/sd_card_storage.h`
- `main/storage/sd_card_storage.cc`
- `main/audio/local_recording_service.cc`
- `main/boards/common/audream_device_control.cc`
- `main/boards/common/blufi.cpp`
- `main/boards/waveshare/esp32-s3-touch-amoled-2.06/esp32-s3-touch-amoled-2.06.cc`
- `main/CMakeLists.txt`
- `sdkconfig.defaults.esp32s3`

当前行为：

- 设备启动后自动 probe SD card。
- 若挂载成功，会准备 `/sdcard/audream/recordings`。
- storage status 会返回：
  - mounted
  - total bytes
  - free bytes
  - recording bytes
  - estimated remaining minutes
  - error code
- 录音开始前只读取 storage cached status，不在录音路径里临时 mount。
- iOS 可主动 refresh storage status，触发重新 probe。

## 验证步骤

### 编译前配置刷新

如果 `sdkconfig.defaults.esp32s3` 已更新，但 `build/config/sdkconfig.h` 没有反映 LFN 配置，执行：

```bash
source ~/.espressif/tools/activate_idf_v5.5.4.sh
idf.py reconfigure
```

随后正常编译烧录：

```bash
idf.py build flash monitor
```

### 期望日志

启动后应看到类似：

```text
AudreamStorage: Starting startup SD card initialization
AudreamStorage: Mounting SD card in SDMMC 1-bit mode with clk=2 cmd=1 d0=3
AudreamStorage: Calling esp_vfs_fat_sdmmc_mount at /sdcard
SD card mounted at /sdcard
Created storage directory: /sdcard/audream/recordings
Startup SD card initialization completed
```

如果目录已经存在，则不会重复打印 created directory，但 storage status 应为 mounted。

### iOS 侧验证

1. 打开 App 后自动连接 Audream Watch。
2. 进入 device detail。
3. 查看剩余空间。
4. 若状态异常，触发 refresh/remount。
5. 点击 Record on Audream Watch。
6. 停止录音后检查本地录音列表。

## 常见错误和判断

### `sdmmc_card_init failed (0x107)`

通常表示卡没有响应或总线模式/引脚不对。

在本板上，如果看到 SDSPI 相关日志，应优先检查是否误走了 SDSPI。Waveshare ESP32-S3 Touch AMOLED 2.06 应使用 SDMMC 1-bit。

### `BTC_TASK stack overflow`

说明 BLE 回调里执行了重任务。SD mount、文件系统扫描、长耗时控制逻辑都不能直接放在 BLE callback task 中执行。

### `Failed to create directory ... Invalid argument`

如果 SD 已挂载但长目录名失败，优先检查 FATFS LFN 配置：

```text
CONFIG_FATFS_LFN_HEAP=y
CONFIG_FATFS_MAX_LFN=255
```

### `Size: luMB`

这是 `sdmmc_card_print_info()` 输出格式在当前环境下的显示问题，不影响挂载成功与否。后续如果需要准确显示容量，应使用 `f_getfree()` 计算出来的 total/free bytes。

## 后续工作

- 重新编译烧录后确认 LFN 是否解决目录创建失败。
- 完成一次设备本地 OPUS 录音写入测试。
- 完成 local recordings list/read chunk BLE 链路测试。
- 评估长录音时的写入稳定性和剩余空间更新频率。
- 后续第二版再接入双麦录音；当前阶段先保持 mono OPUS。
