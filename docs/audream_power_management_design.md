# Audream Note Watch 电源管理设计

## 目标

设备应优先保证录音和同步可靠性，同时在空闲时自动降低功耗。电源管理由固件主导，iOS 的 BLE background mode 只用于提高重连和后台同步成功率，不应导致设备永久保持唤醒。

状态机必须严格按以下顺序迁移：

```text
Active -> Light Sleep -> Deep Sleep
```

不得因为未同步录音超时而从 `Active` 直接进入 `Deep Sleep`。

## 状态定义

### Active

`Active` 是完整工作状态。设备可以录音、读写 SD 卡、通过 BLE 控制、通过 BLE 或 Wi-Fi 同步录音文件。

进入 `Active` 的有效活动包括：

- 用户按键。
- 开始录音。
- 停止录音。
- BLE 文件读取，例如 `readLocalRecordingChunk`。
- Wi-Fi 快传启动或完成，例如 `startTemporaryAPTransfer`、`completeTemporaryAPTransfer`。
- iOS 前台明确控制操作，例如修改麦克风增益、录音模式、重置设置。
- 设备详情页前台主动刷新，如果协议能够区分 foreground control。

不应把后台维护活动算作有效活动：

- `listLocalRecordings`。
- 普通 `getDeviceState`。
- `syncTime`。
- BLE keepalive。
- BLE reconnect。
- iOS 后台 auto-sync 的空闲轮询。

这些后台维护活动可以被处理，但不能刷新 `Active` 的 idle timer，也不能让 `Light Sleep` 回到 `Active`。

### Light Sleep

`Light Sleep` 是短期空闲等待状态，用于保留快速恢复能力。

进入条件：

- 没有正在录音。
- 没有正在进行 BLE 文件读取。
- 没有正在进行 Wi-Fi 快传。
- 没有 SD 卡写入、metadata flush 或 mount/probe。
- 超过 `Active` idle timeout。

推荐 timeout：

- 普通空闲：45 秒。
- 停止录音后有未同步文件：保留 120 秒 `Active` 同步窗口，然后进入 `Light Sleep`。
- Wi-Fi 快传完成：15-30 秒后可进入 `Light Sleep`。

在 `Light Sleep` 中，如果发生有效活动，立即回到 `Active`。

如果只发生后台维护活动，例如 `listLocalRecordings` 或 `syncTime`，可以短暂处理，但保持 `Light Sleep`，且不重置 180 秒计时。

### Deep Sleep

`Deep Sleep` 是最低功耗状态。进入后 BLE 和 Wi-Fi 都不可用，iOS 无法主动唤醒设备。

进入条件：

- 已处于 `Light Sleep`。
- `Light Sleep` 连续持续 180 秒。
- 期间没有有效活动使设备回到 `Active`。
- 当前没有录音。
- 当前没有 BLE/Wi-Fi 文件传输。
- 当前没有 SD 卡写入、metadata flush 或 mount/probe。
- 当前不是 USB 供电或调试连接。

进入前动作：

- 如果 BLE 仍连接，向 iOS 发送 `deviceWillSleep` 事件。
- 等待约 500 ms，让 iOS 更新 UI 和停止轮询。
- 主动断开 BLE。
- 停止 Wi-Fi STA/AP。
- 关闭 codec、麦克风输入、屏幕和非必要外设。
- flush 必要 metadata。
- 配置按键 GPIO wakeup。
- 进入 deep sleep。

唤醒方式：

- 第一版只使用用户按键唤醒。
- RTC 定时唤醒暂不启用，避免增加功耗和行为复杂度。

## USB 供电规则

USB 供电或调试连接时，不自动进入 `Deep Sleep`，避免串口断开、烧录和 monitor 被影响。

USB 不阻止进入 `Light Sleep`。如果设备已在 `Light Sleep` 且达到 180 秒，但检测到 USB 供电，应停留在 `Light Sleep`。

建议日志节流输出：

```text
Deep sleep blocked by USB power
```

不要每秒刷屏。建议同一原因至少间隔 60 秒再打印一次。

如果 USB 拔掉，设备仍处于 `Light Sleep` 且已经满足 180 秒条件，则下一次 power manager tick 可以进入 `Deep Sleep`。

## 未同步录音策略

停止录音后，如果设备上存在未同步录音：

```text
Stop recording
-> Active 120 秒同步窗口
-> 未开始同步或同步失败
-> Light Sleep
-> Light Sleep 连续 180 秒无有效活动
-> Deep Sleep
```

未同步录音只能延长 `Active` 同步窗口，不能让设备绕过 `Light Sleep` 直接进入 `Deep Sleep`。

如果 `Light Sleep` 中 iOS 开始真实同步：

```text
Light Sleep
-> readLocalRecordingChunk 或 startTemporaryAPTransfer
-> Active
-> 传输完成
-> Active idle timeout
-> Light Sleep
-> 180 秒无有效活动
-> Deep Sleep
```

## iOS 轮询配合

iOS 可以继续使用 BLE background mode，但后台轮询不能阻止设备进入低功耗状态。

建议：

- App 前台设备页：可 5-10 秒轮询。
- App 后台：使用退避轮询，例如 10 秒、30 秒、60 秒、120 秒。
- 收到 `deviceWillSleep` 或 BLE 断开后，停止密集轮询。
- 用户再次打开 App 时重新扫描并连接设备。

`deviceWillSleep` 使用现有 BLUFI custom data 通道发送单向事件，不占用 request/response：

```text
[version=0x01, event_opcode=0xF0, event_id=0x01]
```

iOS 收到该事件后应停止 `DeviceRecordingAutoSyncService`，标记设备断开，取消当前 BLE 连接，并等待用户操作或系统重连。

## 固件实现建议

新增 `AudreamPowerManager`，集中维护：

- 当前状态：`Active`、`LightSleep`、`DeepSleepPreparing`。
- 最后有效活动时间。
- 进入 `Light Sleep` 的时间。
- 当前阻塞原因：录音、BLE 传输、Wi-Fi 快传、SD 写入、USB 供电等。

需要提供两类 activity API：

```text
NotifyUserActivity(reason)
NotifyMaintenanceActivity(reason)
```

`NotifyUserActivity` 刷新 active idle timer，并可使 `Light Sleep` 回到 `Active`。

`NotifyMaintenanceActivity` 只记录诊断信息，不刷新 active idle timer，不使 `Light Sleep` 回到 `Active`。

需要提供 blocker API：

```text
SetRecordingActive(bool)
SetBleFileTransferActive(bool)
SetTemporaryAPTransferActive(bool)
SetStorageBusy(bool)
SetUsbPowered(bool)
```

Power manager 每 1 秒 tick 一次即可。日志需要节流，避免影响功耗和串口可读性。

## 第一版实施范围

第一版只实现：

- Active / Light Sleep / Deep Sleep 状态机。
- 有效活动与后台维护活动区分。
- 停止录音后的 120 秒同步窗口。
- Light Sleep 180 秒后尝试 Deep Sleep。
- USB 供电阻止 Deep Sleep。
- 按键唤醒。

暂不实现：

- RTC 定时唤醒。
- 根据电量动态调整 timeout。
- iOS 主动配置 timeout。
- 云端策略下发。
