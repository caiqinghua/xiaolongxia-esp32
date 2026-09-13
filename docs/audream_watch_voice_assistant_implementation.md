# Audream 手表与语音助手实现方案

状态：Draft

更新日期：2026-09-13

适用工程：`firmware/note-watch-206`、`server`、拟新增的 `audream-voice-server`

## 1. 背景

`note-watch-206` 当前在 Waveshare ESP32-S3 Touch AMOLED 2.06 硬件上主要运行 Audream 本地录音模式。工程源自小智 ESP32 项目，原有实时语音助手能力仍保留在代码中，但当前板级实现通过 `IsRecordingOnlyMode()` 进入录音专用启动分支，跳过了完整的联网、唤醒词、AFE、实时会话和显示交互流程。

本次目标不是用手表功能替换原有语音助手，也不是把设备改成只运行唤醒词。目标是把以下三类能力完整组合到同一个产品中：

1. Audream 长按录音、标记、低功耗、同步和 Note 处理流程。
2. 原小智实时语音助手能力，并统一改造成 Audream/追音语音助手。
3. 真正可用的手表基础功能，包括表盘、时间、电量、通知、计时和运动信息。

## 2. 核心决策

### 2.1 产品统一，运行服务隔离

Audream API 与实时语音助手使用同一套账号、设备归属、权限和产品设置，但运行在两个独立服务中：

- `audream-api`：负责账号、设备管理、Notes、长音频转录、纠错、Insights、API Key 和后台任务。
- `audream-voice-server`：负责设备实时连接、流式音频、VAD、流式 ASR、对话 LLM、工具调用、流式 TTS 和打断。

不把原小智实时服务直接合并到当前 `server/api.py`。长音频后台任务具有明显的 CPU、GPU 和内存波动，实时语音会话则要求稳定的低延迟、持续连接和独立背压。进程隔离可以避免 Note 处理拖慢实时语音。

初期两个服务可以部署在同一台服务器，但必须使用独立的 systemd unit 或容器、独立端口、独立健康检查和资源限制。生产规模扩大后可以直接迁移 `audream-voice-server`，而不改变设备协议和公共域名。

建议公共入口：

- REST API：`https://api.audream.ai`
- 实时语音：`wss://voice.audream.ai/v1/device`

### 2.2 保留原语音助手，不保留原管理系统

从 `/root/xiaozhi-esp32-server` 迁移以下运行能力：

- WebSocket 设备会话和二进制 Opus 音频传输。
- `hello`、`listen`、`abort`、IoT 和 MCP 等协议能力。
- VAD、ASR、LLM、TTS provider 抽象。
- 流式输出、打断、会话上下文、工具调用和设备状态上报。
- 必要的音频编解码、队列、背压和超时控制。

不继续使用以下独立系统：

- 原 manager API 的用户和设备归属体系。
- 原 manager web/mobile 管理前端。
- 原独立 OTA 所有权和静态认证方式。
- 原小智品牌、域名和面向用户的名称。

迁移必须从已确认的上游提交创建干净分支。生产目录中的本地修改不能直接覆盖或丢弃，应先导出 diff，逐项审查后再迁移。当前确认的参考提交为：

```text
435860d339d0efab07f1555560f5bacd363e92cf
```

### 2.3 本地录音优先级最高

长按录音是现有核心行为，必须在任何语音助手状态下可靠执行。音频资源由统一协调器独占管理，禁止本地录音和语音助手同时读取麦克风、编码 Opus 或播放 TTS。

优先级从高到低：

1. 低电量安全停止与文件落盘。
2. 用户长按发起的本地录音。
3. 文件传输。
4. 实时语音助手。
5. 后台唤醒检测。

## 3. 范围

### 3.1 本期实现范围

- 为板级代码引入组合运行模式，不再把 Waveshare 手表固定为录音专用模式。
- 保留长按开始/结束录音和录音中单击标记。
- 恢复并接入原实时语音助手链路。
- 新增音频会话协调器，解决录音、AFE、唤醒词、TTS 和文件传输冲突。
- 建立 Audream 设备注册、设备凭证和语音助手设置。
- 将原实时服务迁移为独立的 `audream-voice-server`。
- 实现手表基础 UI、RTC、IMU、通知和快捷设置。
- 提供常开语音与省电语音两种功耗模式。
- 完成可观测性、压力测试、功耗测试和回归测试。

### 3.2 非目标

- 不在第一阶段重写 ASR、LLM 或 TTS 引擎。
- 不把长音频转录改成实时流式识别。
- 不默认保存实时语音的原始音频。
- 不让设备持有用户的 Audream API Key。
- 不在第一阶段移除所有原小智内部类名；先完成公共品牌、协议入口和服务边界迁移，再逐步清理内部命名。
- 不承诺深度睡眠状态下运行 ESP32-S3 AFE 唤醒词。

## 4. 当前实现基线

### 4.1 固件启动分支

当前 `main/application.cc` 在 `board.IsRecordingOnlyMode()` 为 `true` 时执行本地录音专用初始化后直接返回，因此不会进入完整的显示、网络、音频处理器、唤醒词和实时协议初始化。

Waveshare 板级类当前：

- 继承 `WifiBoard`。
- `IsRecordingOnlyMode()` 返回 `true`。
- `StartNetwork()` 只启动 Audream BLE 控制流程，而不是完整调用 `WifiBoard::StartNetwork()`。
- 已有稳定的长按录音、录音标记、存储检查、低电量停止和休眠唤醒逻辑。

因此不能只把 `IsRecordingOnlyMode()` 改为 `false`。这样虽然可能进入原助手流程，但会绕开现有录音专用初始化假设，并引入 BLUFI 生命周期、音频资源和休眠策略冲突。

### 4.2 音频能力

完整模式下的 `AudioService` 已经具备语音助手与本地录音的基础共存能力：

- 初始化编码器、解码器、重采样和 AFE/audio processor。
- 输入任务能够优先把本地录音 PCM 送入本地 Opus 编码路径。
- 本地录音服务已经通过 `EnableLocalRecording()`、packet callback 和 metadata 生成文件。

缺少的是明确的会话仲裁、状态切换和队列清理，而不是重新实现一套 AudioService。

### 4.3 设备硬件

目标板硬件能力包括：

- 410 x 502 AMOLED 触摸屏。
- FT3168 触摸控制器。
- QMI8658 IMU。
- PCF85063 RTC。
- AXP2101 电源管理。
- TF 卡。
- 双数字麦克风、ES7210/ES8311 音频链路和扬声器。

目前 RTC 和 IMU 尚未形成完整的手表服务层，需要新增驱动适配和领域服务，不能把寄存器访问散落在 UI 中。

### 4.4 服务端基线

当前 Audream 服务已经具备账号、会话/API Key、Note、转录、Insights 和任务处理能力，但没有完整的云端设备注册和实时设备凭证体系。

原实时语音服务具备独立 WebSocket 和 OTA HTTP 服务，支持多种 provider、MCP、IoT 和打断，但其账号、设备绑定和配置依赖原 manager API。迁移时需要替换这一依赖，而不是把两个数据库并存。

## 5. 总体架构

```text
                                +------------------------+
                                |     Audream clients    |
                                |  iOS / Android / Web   |
                                +-----------+------------+
                                            |
                         HTTPS              | HTTPS
                                            v
+-------------------+      WSS      +------------------------+
| Audream Watch     +-------------->+ audream-voice-server   |
|                   |               |                        |
| Watch UI          |               | Session gateway        |
| Local recorder    |               | Streaming ASR / VAD    |
| Voice assistant   |               | LLM / MCP / tools      |
| BLE / Wi-Fi       |               | Streaming TTS          |
+---------+---------+               +-----------+------------+
          |                                         |
          | BLE / temporary AP                      | internal HTTPS
          v                                         v
+-------------------+                     +------------------------+
| Mobile importer   +-------------------->+ audream-api            |
| and notifications |                     |                        |
+-------------------+                     | Accounts and devices   |
                                          | Notes and processing   |
                                          | Settings and API keys  |
                                          +------------------------+
```

服务端只统一领域数据和身份，不共享实时连接进程。实时服务通过内部接口读取设备授权和写入会话事件，不能直接访问 Audream API 的内部 Python 对象或任务队列。

## 6. 固件运行模式

### 6.1 引入运行 Profile

使用明确的运行 profile 替代单一的 `IsRecordingOnlyMode()` 布尔值：

```cpp
enum class BoardRuntimeProfile {
    kAssistant,
    kRecordingOnly,
    kWatchAssistantRecorder,
};
```

建议默认映射：

| 设备 | Profile | 行为 |
| --- | --- | --- |
| 原语音助手开发板 | `kAssistant` | 保持原助手流程 |
| StickS3 endurance 测试设备 | `kRecordingOnly` | 保持录音续航测试流程 |
| Waveshare 2.06 手表 | `kWatchAssistantRecorder` | 手表、助手和本地录音组合流程 |

为避免一次性修改所有板级类，可以先保留 `IsRecordingOnlyMode()` 兼容方法，由默认实现映射到 profile，随后逐步迁移调用点。

### 6.2 组合模式启动顺序

`kWatchAssistantRecorder` 建议按以下顺序启动：

1. 初始化电源、RTC、显示、触摸、按键和存储。
2. 读取上次关机状态、录音 metadata 和用户功耗设置。
3. 初始化完整 `AudioService::Initialize()`，而不是 local-recording-only 初始化。
4. 注册 `LocalRecordingService` 回调，但暂不占用输入设备。
5. 初始化 `AudioSessionCoordinator`。
6. 初始化 Wi-Fi、BLE 控制和设备凭证。
7. 根据功耗模式决定是否启动 AFE 和唤醒词。
8. 启动 Watch UI 和后台服务。
9. 进入 `idle`，等待长按录音、触摸操作或唤醒词。

任何步骤失败都必须降级到仍可录音的模式。显示、IMU、Wi-Fi 或语音服务故障不能阻止本地录音。

## 7. 音频会话协调器

### 7.1 状态模型

现有 `DeviceState` 继续表示语音助手协议和 UI 状态。新增独立的音频资源状态，避免把录音、传输和语音助手全部塞进原状态机：

```cpp
enum class AudioSessionMode {
    kIdle,
    kAssistantConnecting,
    kAssistantListening,
    kAssistantThinking,
    kAssistantSpeaking,
    kLocalRecording,
    kFileTransfer,
};
```

协调器建议暴露以下接口：

```cpp
class AudioSessionCoordinator {
public:
    bool RequestAssistantStart();
    void RequestAssistantStop();
    bool RequestLocalRecordingStart();
    void RequestLocalRecordingStop();
    bool BeginFileTransfer();
    void EndFileTransfer();
    void PrepareForSleep();
    AudioSessionMode mode() const;
};
```

所有 codec、microphone input、speaker output、AFE、Opus encoder 和 audio queue 的启停必须经过协调器。板级按钮不得直接拼接多组启停调用。

### 7.2 状态切换规则

| 当前状态 | 事件 | 结果 |
| --- | --- | --- |
| Idle | 长按 | 启动本地录音 |
| Assistant listening | 长按 | 发送 abort，停止助手，启动本地录音 |
| Assistant speaking | 长按 | 立即停止 TTS，发送 abort，启动本地录音 |
| Assistant thinking | 长按 | 取消请求，启动本地录音 |
| Local recording | 单击 | 添加 highlight |
| Local recording | 长按 | 完成本地录音并落盘 |
| Local recording | 唤醒词 | 忽略 |
| File transfer | 助手请求 | 拒绝并显示忙碌状态 |
| File transfer | 长按 | 默认拒绝；若产品要求强制录音，则先安全中止传输 |
| Any | 电量低于安全阈值 | 停止音频、写 metadata、准备关机 |

### 7.3 从助手切换到录音

切换顺序必须固定：

1. 阻止新语音输入和新 TTS packet 入队。
2. 向服务器发送 `abort`，但不等待网络确认。
3. 停止扬声器并清空 decoder/output queue。
4. 停止 AFE/wake processor 消费输入。
5. 清空残留 assistant encoder queue。
6. 启动本地录音文件和 metadata transaction。
7. 打开录音输入并开始本地 Opus 编码。
8. 更新 UI、LED 和电源 blocker。

整个切换不得依赖服务器在线。目标是从长按确认到开始保留 PCM 的延迟不超过 300 ms，并通过预卷缓冲避免丢失长按前后的首句音频。

### 7.4 从录音恢复助手

1. 停止录音输入并完成剩余 packet 编码。
2. 原子更新 segment 和 session metadata。
3. 关闭录音文件并释放录音电源 blocker。
4. 恢复完整 AudioService 输入路径。
5. 根据用户功耗模式恢复 AFE/wake processor。
6. 网络可用时恢复或重新建立实时连接，但不自动开始对话。

## 8. 按键、触摸和交互

### 8.1 物理按键

保留现有行为：

- 长按 1.5 秒：开始或结束本地录音。
- 录音中单击：添加 highlight。
- 屏幕关闭时单击：只唤醒屏幕，不启动助手。

建议新增但不与本地录音冲突的行为：

- 双击：可配置为打开语音助手或最近录音。
- 关机菜单：通过长按更长时间或快捷设置进入，不能复用 1.5 秒录音动作。

### 8.2 语音助手入口

语音助手有两个入口：

- 表盘或应用列表中的麦克风按钮，使用 push-to-talk，第一阶段优先实现。
- 用户开启常开语音后使用本地唤醒词。

面向用户统一显示 `Audream Voice Assistant` 或本地化的“追音语音助手”。如果需要新的唤醒短语，必须训练或获得对应的设备端 wake model；只修改文本和提示音不会改变唤醒词。

### 8.3 手表页面

第一版页面结构：

1. 表盘：首页显示时间、日期、电量、连接状态和录音状态。
2. 录音：开始/停止、时长、标记、剩余容量和同步状态。
3. 语音助手：会话状态、识别文本、停止和打断。
4. 通知：最近通知列表与详情。
5. 计时：闹钟、计时器和秒表。
6. 活动：步数和基础活动信息。
7. 最近录音：本地文件、时长、同步结果。
8. 快捷设置：亮度、音量、Wi-Fi、BLE、助手模式和省电模式。

## 9. 手表领域服务

UI 不直接操作硬件寄存器。新增以下服务边界：

### 9.1 `RtcService`

- 封装 PCF85063 初始化、读写和闹钟中断。
- 网络同步时间后写入 RTC。
- 无网络时使用 RTC 保持时间。
- 提供本地时区和 12/24 小时格式给 UI。

### 9.2 `MotionService`

- 封装 QMI8658 初始化和采样。
- 支持抬腕亮屏、步数和静止检测。
- 通过低频任务或中断运行，禁止 UI 高频轮询 I2C。
- 原始运动数据默认不上传服务器。

### 9.3 `NotificationService`

- 手机通过 BLE 发送通知摘要。
- 手表只保存有限条最近通知。
- 支持应用名、标题、正文摘要、时间和已读状态。
- 通知内容必须受手机端授权控制，并提供一键清空。

### 9.4 `WatchPowerPolicy`

- 管理屏幕、IMU、Wi-Fi、BLE、AFE 和 CPU profile。
- 根据录音、助手、传输和充电状态设置 blocker。
- 统一决定 light sleep/deep sleep，避免多个模块分别调用睡眠 API。

## 10. 网络与 BLUFI

### 10.1 两类 Wi-Fi 必须分离

设备存在两类完全不同的 Wi-Fi 用途：

- Internet STA：连接路由器，用于实时语音助手、OTA 和时间同步。
- Temporary transfer AP：设备临时开启 AP，由手机连接后同步录音文件。

两者不能共享一个模糊的 `connected` 状态。建议引入：

```cpp
enum class NetworkPurpose {
    kNone,
    kInternet,
    kFileTransfer,
};
```

文件传输期间暂停实时语音连接。传输结束后恢复 Internet STA，并由手机恢复原办公室 Wi-Fi；设备端不能把临时 AP 当成 Internet 网络。

### 10.2 BLUFI 生命周期冲突

当前板级 `StartNetwork()` 使用 `Blufi::init(false)` 进入 Audream control-only 模式，而 `WifiBoard::StartNetwork()` 的现有连接流程可能在联网后调用 `Blufi::deinit()`。直接同时调用两者会导致：

- 配网完成后 Audream BLE 控制通道被关闭。
- 重复初始化 singleton。
- 录音同步事件与配网事件竞争 callback。

需要把 BLUFI 拆成两个明确角色：

```cpp
enum class BlufiRole {
    kProvisioning,
    kAudreamControl,
    kProvisioningAndControl,
};
```

实现要求：

- 未配置 Wi-Fi 时启用 provisioning + control。
- 配网成功后只关闭 provisioning 能力，保留 control service。
- 进入临时文件传输时不销毁 BLE 控制状态。
- 重连和休眠恢复必须幂等。
- Wi-Fi SSID/password 只保存在设备 NVS，不上传 Audream 服务。

## 11. 功耗模式

### 11.1 Always Available

适用于充电或用户明确开启常开语音：

- Wi-Fi 保持可恢复的在线状态。
- AFE 和 wake word 持续运行。
- 屏幕按超时关闭。
- 使用 light sleep/动态频率，不进入会停止 AFE 的 deep sleep。

该模式必须在 UI 中明确显示更高耗电，不可将其作为默认省电模式。

### 11.2 Battery Saver

默认建议模式：

- 屏幕关闭后允许 deep sleep。
- 不在 deep sleep 中承诺唤醒词。
- 用户通过按键或触摸唤醒设备，再使用 push-to-talk。
- Wi-Fi 按需连接，BLE 使用低占空比。
- 本地录音开始后沿用当前关闭屏幕、暂停非必要无线模块和降低 CPU profile 的逻辑。

### 11.3 Recording

- 本地录音持有录音 blocker。
- 关闭屏幕和扬声器。
- 停止实时语音 WebSocket、TTS 和 wake processor。
- BLE 是否保留由同步/控制需求决定，但必须沿用已验证的后台低功耗策略。
- 低于 5% 电量时安全停止，确保 metadata 完成。

### 11.4 功耗验收

每种模式需要使用同一硬件和同一电池条件测量：

- 表盘常亮。
- 表盘熄屏待机。
- Always Available 熄屏待机。
- Battery Saver deep sleep。
- 本地录音。
- 实时语音对话。
- 临时 AP 文件传输。

文档记录平均电流、峰值电流、测试时长、固件 commit、电池电压和无线信号条件，不能只用电量百分比估算。

## 12. 实时语音服务

### 12.1 目录与模块

建议新建独立仓库或顶层服务目录 `audream-voice-server`：

```text
audream-voice-server/
  audream_voice/
    transport/
    sessions/
    audio/
    providers/
      asr/
      llm/
      tts/
      vad/
    tools/
    mcp/
    auth/
    events/
    observability/
  tests/
  deploy/
  pyproject.toml
```

职责划分：

- `transport`：WebSocket、消息解析、兼容协议和连接保活。
- `sessions`：每设备会话状态、取消、打断和超时。
- `audio`：Opus、VAD、缓冲、背压和时间戳。
- `providers`：ASR、LLM、TTS、VAD provider adapter。
- `tools`/`mcp`：设备工具、Audream MCP 和外部 MCP。
- `auth`：设备 JWT 验证和权限。
- `events`：向 `audream-api` 写入会话摘要和事件。
- `observability`：结构化日志、指标和 tracing。

### 12.2 协议迁移

第一阶段保持原设备协议兼容，使现有固件可以先验证新服务。新增能力通过协议版本协商启用：

```json
{
  "type": "hello",
  "version": 2,
  "transport": "websocket",
  "features": {
    "local_recording": true,
    "barge_in": true,
    "mcp": true
  }
}
```

协议迁移原则：

- 不在同一版本中改变二进制音频格式。
- 每个消息定义 schema 和最大尺寸。
- 所有长任务可取消。
- 断线后不自动重放用户音频。
- 旧协议设定明确的兼容期限和 telemetry。

### 12.3 Provider 策略

实时语音和 Note 长音频处理使用不同 provider 策略：

- 实时 ASR 优先低首包延迟和 partial transcript。
- Note 转录继续使用长音频 FunASR/diarization 管线。
- 实时 LLM 使用流式输出，不能等待完整 JSON 后再启动 TTS。
- TTS 必须支持分句或流式合成和快速取消。
- Provider 故障要支持超时、有限重试和降级，不能阻塞整个 event loop。

## 13. Audream API 扩展

以下接口为拟新增接口，不代表当前已经存在。

### 13.1 设备注册

```http
POST /v1/devices/register
Authorization: Bearer <user-session>
```

请求：

```json
{
  "device_id": "watch-device-uuid",
  "model": "waveshare-s3-touch-amoled-2.06",
  "firmware_version": "2.3.0",
  "proof": "pairing-proof"
}
```

返回设备归属结果和一次性 credential provisioning 数据。设备不能在日志中打印完整 credential。

### 13.2 设备语音 bootstrap

```http
POST /v1/devices/{device_id}/voice/bootstrap
Authorization: Device <device-credential>
```

返回短期 token 和 voice endpoint：

```json
{
  "access_token": "short-lived-device-jwt",
  "expires_in": 3600,
  "websocket_url": "wss://voice.audream.ai/v1/device",
  "protocol_version": 2
}
```

实时网关使用公钥本地验证 JWT，不能对每个音频 packet 回调 `audream-api`。

### 13.3 助手设置

```http
GET /v1/devices/{device_id}/voice-settings
PATCH /v1/devices/{device_id}/voice-settings
```

建议设置模型：

```json
{
  "enabled": true,
  "activation_mode": "push_to_talk",
  "wake_word_enabled": false,
  "language": "zh-CN",
  "voice": "default",
  "save_transcript": true,
  "create_note": false,
  "power_mode": "battery_saver"
}
```

### 13.4 内部事件接口

`audream-voice-server` 使用 service credential 批量写入事件：

```http
POST /internal/v1/voice-events/batch
Authorization: Service <service-token>
```

内部接口不向用户 API Key 开放，并限制来源网络、audience 和速率。

## 14. 数据模型

建议新增以下表，字段名称以实际 ORM 规范为准。

### 14.1 `devices`

- `id`
- `hardware_id`
- `model`
- `firmware_version`
- `status`
- `last_seen_at`
- `created_at`

### 14.2 `device_owners`

- `device_id`
- `user_id`
- `role`
- `paired_at`
- `unbound_at`

必须支持明确解绑。重新配对时，如果设备仍属于其他账号，客户端应提示先解绑，而不是静默覆盖归属。

### 14.3 `device_credentials`

- `device_id`
- `credential_hash` 或 public key
- `version`
- `created_at`
- `revoked_at`

不在数据库保存可直接使用的明文长期 secret。

### 14.4 `voice_assistant_settings`

- `device_id`
- `enabled`
- `activation_mode`
- `wake_word_enabled`
- `language`
- `voice`
- `save_transcript`
- `create_note`
- `power_mode`
- `updated_at`

### 14.5 `voice_sessions`

- `id`
- `device_id`
- `user_id`
- `started_at`
- `ended_at`
- `termination_reason`
- `asr_provider`
- `llm_provider`
- `tts_provider`
- `latency_summary`

### 14.6 `voice_turns`

- `id`
- `session_id`
- `role`
- `text`
- `tool_name`
- `started_at`
- `ended_at`
- `interrupted`

只有用户开启保存记录时才持久化完整 turn 文本。否则仅保存必要的错误和聚合指标。

## 15. 身份、授权和安全

### 15.1 配对和凭证

1. 手机 App 登录 Audream 账号。
2. App 通过 BLE 验证附近设备和物理配对动作。
3. App 调用设备注册接口建立 owner 关系。
4. 服务端签发设备长期 credential 或注册设备公钥。
5. App 通过加密 BLE 会话把凭证写入设备 NVS。
6. 设备使用长期凭证换取短期 device JWT。
7. 实时网关验证 JWT 的 signature、audience、device id、expiry 和 credential version。

### 15.2 安全要求

- 用户 API Key 不下发到设备。
- 设备长期 secret 不写日志、不进入 crash report。
- WebSocket 必须使用 TLS。
- JWT 有短有效期并支持设备解绑后撤销。
- 工具调用按用户、设备和 tool scope 授权。
- IoT/MCP 参数必须 schema 校验，禁止任意 shell 或任意 URL 请求。
- 服务间 token 使用独立 audience，不能作为用户 token 使用。
- OTA 包必须签名并验证型号、版本和 hash。

## 16. 数据与隐私

实时语音默认行为：

- 原始音频只在流式处理内存中存在，处理后释放。
- 默认不写磁盘、不放对象存储、不加入 Note。
- 用户明确开启保存 transcript 时才保存文本。
- 用户明确开启 `create_note` 或在会话中执行保存动作时才创建 Note。
- 调试日志不能包含原始音频、完整 token 或完整用户对话。

创建 Note 时，记录来源为 `voice_assistant`，并保留设备、会话和语言元数据。后续转录、纠错和 Insights 是否执行，遵循用户现有 Auto Summary 设置，不引入重复的 `auto_insight_enabled` 开关。

## 17. 建议代码改动边界

### 17.1 固件

```text
main/
  application.cc
  audio/
    audio_session_coordinator.h
    audio_session_coordinator.cc
  watch/
    rtc_service.h
    rtc_service.cc
    motion_service.h
    motion_service.cc
    notification_service.h
    notification_service.cc
    watch_power_policy.h
    watch_power_policy.cc
  boards/waveshare/esp32-s3-touch-amoled-2.06/
    ...
```

现有 `LocalRecordingService`、`AudioService` 和 power manager 继续复用。协调器只统一编排，不复制编码、文件和功耗实现。

### 17.2 Audream API

按现有服务结构新增 device domain、voice settings、bootstrap 和 internal events。不要继续扩大单一 `api.py`；路由、schema、service 和 repository 应分层，具体命名遵循当前项目已经采用的模块模式。

### 17.3 Voice server

从干净的原服务器 fork 开始，先保留协议和 provider 测试，再逐步替换 manager API、认证、配置和品牌。禁止从正在运行且有未提交改动的生产目录直接 `rsync --delete` 到新仓库。

## 18. 实施阶段

### Phase 0：建立可回退基线

- 固定固件、Audream API 和原语音服务 commit。
- 导出生产目录本地修改清单。
- 保存当前长按录音、同步、低电量和后台功耗测试结果。
- 为现有语音协议建立消息样本和最小集成测试。

完成条件：三套基线均可独立重建，已知生产修改不丢失。

### Phase 1：迁移实时语音服务

- 创建 `audream-voice-server` 干净仓库。
- 保留 WebSocket、VAD、ASR、LLM、TTS、MCP 和打断。
- 移除 manager web/mobile 和原用户设备体系依赖。
- 接入本地开发用的临时设备认证。
- 增加 health、readiness、结构化日志和指标。

完成条件：未修改原助手协议的测试固件可以通过新域名完成一次语音对话和打断。

### Phase 2：统一设备身份

- 在 Audream API 增加设备、owner、credential 和 voice settings。
- 实现配对、解绑、credential rotation 和 device bootstrap。
- Voice server 改为验证 Audream device JWT。
- App 增加助手设置和设备凭证 provisioning。

完成条件：被解绑设备无法建立新会话；同一用户的 App 和设备显示一致设置。

### Phase 3：固件组合模式

- 引入 `BoardRuntimeProfile`。
- 引入 `AudioSessionCoordinator`。
- Waveshare 板使用完整 AudioService 初始化。
- 先实现屏幕按钮 push-to-talk，不启用常开唤醒。
- 保留长按录音、highlight、低电量停止和同步。

完成条件：连续执行助手、打断、录音、停止、再次助手至少 100 次，无死锁、崩溃、损坏文件或资源泄漏。

### Phase 4：唤醒词、AEC 和打断

- 恢复 AFE 和 device AEC 配置。
- 验证麦克风/参考通道顺序。
- 接入 Audream 唤醒词模型。
- 实现说话打断 TTS。
- 增加 Always Available 与 Battery Saver 切换。

完成条件：安静、播放 TTS、近讲和远讲场景均达到既定唤醒率、误唤醒率和打断延迟。

### Phase 5：手表功能

- 完成 Watch UI 框架和表盘。
- 接入 RTC、IMU、通知、闹钟、计时器和秒表。
- 完成最近录音和快捷设置。
- 增加屏幕防残影策略和触摸锁。

完成条件：断网仍能显示正确时间、使用计时功能和本地录音；通知和运动功能故障不影响录音。

### Phase 6：功耗与生产化

- 对所有功耗模式执行仪器测量。
- 完成断网、弱网、provider 超时、服务器重启和 OTA 回归。
- 为实时服务建立容量模型和自动扩容指标。
- 灰度开启新固件和新语音服务。

完成条件：达到定义的延迟、稳定性、功耗和隐私验收标准，并具备一键回退路径。

## 19. 测试方案

### 19.1 固件单元与组件测试

- Runtime profile 默认值和板级覆盖。
- Audio session 合法/非法状态转换。
- 长按和单击事件去抖。
- RTC 时间读写和断网恢复。
- 设备设置持久化和版本迁移。
- JWT 过期、时钟偏差和 credential rotation。
- metadata 原子写入和低电量停止。

### 19.2 固件硬件测试

- 从助手 listening/speaking/thinking 状态长按录音。
- 录音开始和结束首尾音频完整性。
- 录音中忽略唤醒词和禁止 TTS。
- 文件传输与 Internet STA 恢复。
- 配网后 BLE control service 仍可用。
- 100 次睡眠/唤醒、断网重连和录音循环。
- RTC、IMU、触摸、显示和按键并发。
- 5% 电量安全停止和 metadata 完整性。

### 19.3 服务测试

- WebSocket 协议兼容和版本协商。
- 设备 token 伪造、过期、撤销和错误 audience。
- 流式 ASR partial/final 顺序。
- LLM/TTS 取消和 barge-in。
- 慢客户端、队列上限和背压。
- Provider 超时、断流和降级。
- Audream API 不可用时已有会话的有限降级。
- Note 重任务高负载时实时服务 P95 延迟稳定。

### 19.4 端到端场景

1. App 配对手表并下发设备凭证。
2. 手表 push-to-talk 完成语音对话。
3. TTS 播放期间长按，助手立即停止并开始录音。
4. 录音中添加 highlight，结束后 metadata 完整。
5. 手机通过临时 AP 同步录音。
6. 手表恢复 Internet STA 和语音助手。
7. 用户解绑设备，设备的新语音会话被拒绝。

## 20. 可观测性

### 20.1 固件指标和日志

- Runtime profile 和功耗模式。
- Audio session 状态切换及原因。
- 开始保留 PCM 的长按响应延迟。
- 输入/输出队列深度和 dropped frames。
- WebSocket 重连次数和原因。
- 录音 metadata 成功/失败。
- Wi-Fi purpose 和 BLUFI role 变化。

所有日志使用英文，不记录 secret、Wi-Fi 密码或完整对话。

### 20.2 服务指标

- Active WebSocket connections。
- Connection authentication failures。
- First ASR partial latency。
- End-of-speech to first TTS audio latency。
- Barge-in stop latency。
- Input/output queue depth。
- Dropped audio frames。
- Provider error/timeout rate。
- Session reconnect and termination reasons。
- Internal API latency and failures。

建议为 voice server 设置独立告警，不与 Note worker 的队列和内存告警混合。

## 21. 验收标准

### 21.1 录音回归

- 任何助手状态下长按都能在 300 ms 目标内开始本地录音。
- 录音文件可播放，首尾语音无明显截断。
- highlight、metadata、低电量停止和手机同步与现有版本一致。
- 助手和本地录音不会同时占用 codec 或 encoder。
- 录音期间不会播放 TTS，也不会把录音上传到实时语音服务。

### 21.2 语音助手

- Push-to-talk 可完整完成 ASR、LLM 和 TTS。
- 常开模式可用唤醒词进入会话。
- 用户说话可以打断 TTS。
- 网络中断后会话正确结束，设备仍可本地录音。
- 设备解绑或 token 撤销后无法建立新会话。

### 21.3 手表

- 时间、日期、电量和连接状态正确。
- 无网络时 RTC 保持时间。
- 闹钟、计时器、秒表和基础运动信息可用。
- 屏幕、IMU 或通知异常不阻止录音。
- 显示休眠、抬腕和触摸行为一致且无明显误触。

### 21.4 服务质量

- Note 批量处理高负载不造成实时服务不可用。
- 实时首响应和打断延迟满足发布前设定的 P95 指标。
- 单连接内存稳定，无持续增长。
- 原始实时语音默认不落盘。
- 服务支持滚动重启、连接排空和快速回退。

## 22. 风险与约束

| 风险 | 影响 | 缓解措施 |
| --- | --- | --- |
| 直接取消 recording-only 分支 | 音频、网络和休眠冲突 | 使用 runtime profile 和分阶段迁移 |
| BLUFI 配网与控制共用 singleton | 配网后 BLE 控制失效 | 拆分 role 和生命周期，增加硬件回归 |
| AFE/wake 与录音竞争输入 | 丢音、编码异常或死锁 | 统一 AudioSessionCoordinator 独占资源 |
| 实时服务与 Note worker 同进程 | 延迟抖动和 OOM 互相影响 | 独立进程、资源限制和独立指标 |
| 新唤醒词只有文案没有模型 | 无法实际唤醒 | 将 wake model 作为独立交付物验收 |
| Always Available 功耗过高 | 续航明显下降 | 默认 Battery Saver，UI 明确提示 |
| 原服务生产目录有未提交改动 | 迁移遗漏或不可复现 | 干净 fork + 独立导出和审查 patch |
| 实时语音被默认保存 | 隐私和存储风险 | 默认不落盘，显式设置才保存文本/Note |

## 23. Definition of Done

满足以下条件才视为“完整接入”，而不是仅能演示：

- Waveshare 手表运行 `kWatchAssistantRecorder`，StickS3 endurance 行为未被破坏。
- 本地录音全部现有功能通过回归。
- Audream/追音语音助手具有独立生产服务、正式域名和设备认证。
- 原实时助手的 ASR、LLM、TTS、打断和 MCP 核心能力可用。
- App、API、voice server 和固件使用同一设备归属及设置。
- 手表基础页面、RTC、IMU、通知和计时功能完成。
- BLUFI、Internet STA 和临时传输 AP 生命周期已通过真机测试。
- Always Available、Battery Saver、Recording 和 Transfer 均有实测功耗记录。
- 指标、告警、灰度、回退和隐私策略完成。
- 所有新增代码、日志、错误信息和测试名使用英文，用户文案进入 localization/resource 文件。
