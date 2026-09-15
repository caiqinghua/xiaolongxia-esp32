# Signed Firmware Releases

状态：设备端签名清单下载/校验、发布发现 API、设备查询与发布工具已实现，尚未启用生产公钥或部署发布服务。
不代表硬件 Secure Boot、量产签名策略、资产回退或目标固件验收已经完成。

## 信任与入口

- 手表组合模式的 `Application::UpgradeFirmware(url)` 现在接收 **签名清单 HTTPS URL**，不是裸 `.bin` URL；内部使用 `Ota::UpgradeSigned`。
- `Ota::Upgrade` 拒绝手表组合模式的无签名更新。录音专用模式和原助手模式仍使用既有裸镜像更新入口，未强制迁移其他板型。
- `CONFIG_AUDREAM_OTA_PUBLIC_KEY_HEX` 是编译进固件的 Ed25519 发布公钥，64 个小写十六进制字符，默认空。空值拒绝更新。不要复用设备制造身份私钥，也不要使用桌面测试中的公开测试密钥。
- `CONFIG_AUDREAM_OTA_RELEASE_SEQUENCE` 是当前镜像发布序号。设备只接受更大的签名序号；它不是 eFuse 防回滚计数器，不阻止物理重刷或 bootloader 的故障回退。
- 复用已有 libsodium 验证 Ed25519 和 SHA-256；不自行实现密码学算法。原 ESP-IDF 镜像校验、备用分区写入、启动选择仍保留。

## 镜像身份

手表构建在 ESP-IDF `.rodata_custom_desc` 中保留 `audream_release_identity`，紧接 256 字节的
`esp_app_desc_t`。现有 ESP32-S3 链接脚本明确规定这两个描述区的先后顺序；新增链接保留符号
防止未使用数据被移除。当前工程未发现其他自定义应用描述。

自定义描述是 108 字节：`AUDOTA1\0`（8 字节）、小端发布序号（4 字节）、零结尾型号（96 字节）。
型号来自现有 `BOARD_TYPE`；手表板型为 `esp32-s3-touch-amoled-2.06`，不是云端设备 UUID。
发布工具核对传入型号和序号与镜像内描述一致，并从 ESP 描述读取版本。
设备下载时再次核对镜像内型号、序号和版本与已签名清单一致。
将来更改链接布局或引入第二份自定义描述必须重新验收，不能假定偏移不变。

## 清单协议

外层 JSON 只有两个字符串字段：`payload` 和 `signature`。签名覆盖：

```text
UTF8("audream-firmware-release-v1\n") || UTF8(decoded_payload_string)
```

`signature` 为 64 字节 Ed25519 签名的小写 hex。`payload` 是另一份 JSON 的完整字符串，
不由设备重新序列化；内部恰好有七个字符串字段：

```json
{
  "schema": "audream-firmware-v1",
  "model": "esp32-s3-touch-amoled-2.06",
  "version": "2.2.3",
  "sequence": "1",
  "size": "1234567",
  "sha256": "<64 lowercase hexadecimal characters>",
  "url": "https://updates.example/firmware.bin"
}
```

示例尺寸和 hash 仅展示字段，不是可发布清单。外层最多 4096 字节、payload 最多 2048 字节。
拒绝重复/未知字段、嵌套对象、数组、NUL、非规范十进制数和超限值。
设备签名校验先于内部 payload 解析；服务端先有界解析以选择固定型号公钥，再验签。
只允许无用户信息、query、fragment 的 HTTPS 地址。

先核对 manifest 响应的状态和完整长度，再验签及检查型号/序号，之后才下载镜像。
镜像 HTTP 长度必须等于签名尺寸且适合备用分区；流式 SHA-256 必须与签名 digest 一致，
否则释放写句柄、不选择新分区。校验途中可能已写入备用分区，不承诺无 Flash 写入。
仅在 SDK 镜像校验和启动分区选择成功后报告 100%。

## 发布工具

先由获授权的发布流程构建和验证目标镜像，并妥善管理独立 Ed25519 PEM 私钥。
本工具不创建密钥、不编译、不烧录、不上传，也不覆盖已有输出文件。
私钥应在仓库外，POSIX 权限仅允许文件所有者访问。

```bash
uv run --no-project --with cryptography python tools/firmware_release/sign_release.py \
  --image /path/to/verified/xiaozhi.bin \
  --key /secure/path/release-signing.pem \
  --model esp32-s3-touch-amoled-2.06 \
  --sequence 1 \
  --url https://updates.example/releases/1/firmware.bin \
  --output /path/to/publish/release.json
```

`--sequence` 必须与该镜像构建时的配置一致。发布时使用不可变镜像地址，先提供完整镜像，
再发布对应清单。发现 API 与原子目录发布工具见 ispeak 的 `server/docs/firmware-releases.md`；
发布审批、撤销/密钥轮换操作流程尚未完成。

## 发现与版本确认

`Ota::CheckSignedRelease` 按 `CONFIG_AUDREAM_OTA_API_ORIGIN` 查询
`/v1/firmware/releases/{model}/after/{sequence}`，不要求用户账号或云端授权。
仅 204 代表没有更新；404/503 或无效签名不会伪装成检查成功。取消释放请求并丢弃结果，
但底层阻塞 I/O 仍须等待超时/返回，不能马上将其占用的资源交给其他任务。

查询保存原始已验证清单，安装使用 `Ota::UpgradeSignedEnvelope` 再次验签并下载同一镜像，
不重新读取可变化的 latest URL。页面及后台工作器已接入 Application 协调器的更新
资源，不在 UI 线程执行同步网络操作。长时间下载不能使用不可抢占的
`kMaintenance`，否则会破坏长按录音优先级。

根仓库 `scripts/test_firmware_release_contract.sh` 把真实 FastAPI 响应和对应合成镜像送入
设备查询/安装方法。网络和 Flash 为替身，不会编译或烧录目标固件。

## 后台更新控制器

`watch_update::Controller` 拥有单个可 join 的 pthread，检查/安装不在调用者线程执行。
复用项目已有后台请求的 acquire/join/release 生命周期约定，但不将固件结果放入语音
bootstrap/settings 的 response variant。acquire/release hooks 在 owner 上执行，网络和 Flash
在工作器上执行；最终 pin_commit hook 在工作器内执行一次原子准入，不操作慢速驱动。

检查成功保留具体签名清单，确认须匹配本次 attempt 和版本序号，并在两分钟内完成。
重新检查、取消、时间倒退或过期使旧确认失效。进度回调最多显示 99%，安装成功返回且
工作器退出后才报告启动分区已选择/需要重启；控制器不会直接重启设备。

`InstallSignedEnvelope` 返回安装结果和 OTA 句柄释放确认；原 `UpgradeSignedEnvelope` 保留布尔
兼容包装，新后台工作器必须使用前者。两者增加取消检查和 `before_commit` 准入回调。下载读写间、SDK end 前后
都可取消，作用域负责中止仍有效的 OTA 句柄。控制器在同一个 mutex 内串行化取消与最终
准入：一旦开始 `esp_ota_set_boot_partition`，迟到取消不能覆盖结果。切换失败标记
`kCommitUncertain`，不承诺启动分区未改变，也不允许直接重试更新。

最终准入还必须调用非空的 `pin_commit` hook；没有该 hook 时拒绝确认安装。它必须使用
原协调器的 `BeginFirmwareCommit(generation)` 原子核对并锁定安装所持代次，不得阻塞或
回调控制器。若录音已抢走代次，即使取消通知尚未送达，也不能切换启动分区。

`kFirmwareUpdate` 允许录音/传输抢占；新任务仍须等待实际工作器 join 和资源释放才可使用
硬件。`kFirmwareCommit` 不可抢占，不能通过普通 idle 转移、fault cleanup 或释放更新
回到空闲，提交成功或结果不确定均保留到重启。旧代次释放不能清除新录音的 reservation。

取消不强制杀工作器、不跨线程关闭 HTTP。`StopWithin` 超时保持 active 和资源；必须等
底层 I/O 退出并 join 后才 release。释放失败保留 `kResourceFault`，若已选择启动分区仍
保留 `boot_selected`，不能伪装成普通取消。析构等待工作器，不允许销毁仍被线程访问的数据。
`StopWithin` 只有在可安全交接时返回 true：已 join 但资源 fault 或已经进入提交仍返回 false。
检查线程是否退出使用 `Read().active`，不能循环等待已提交更新的 StopWithin 变为 true。
若 SDK abort 返回失败，`InstallResult.resources_released` 为 false；传递给 owner release
hook，要求保留协调器 fault/lease，不能把 join 成功当作 Flash 句柄已释放。该字段报告 OTA
资源收尾，不替代 owner 对音频、Wi-Fi、功耗及硬件资源的独立核对。

Application 在组合模式初始化控制器，并在主循环推进更新、发布快照及处理四种更新请求。
`watch_firmware_application.cc` 将 hooks 接到原协调器、网络电源计数和存储休眠保护；录音/
传输原有的 owner handoff 会等待更新停止，再执行其余音频清理。失败不会伪装成可开始录音。

联网准备最长 90 秒，继续使用原 station policy；不依赖语音开关或云端账号绑定。准备期间
暂停新的助手 bootstrap 和唤醒监听，复用现有监听退场流程，且保留网络电源计数防止深睡。
检查结束释放临时网络需求，按原功耗策略决定断开 STA 或保持常开；再次确认安装可重连，
但不延长原始两分钟版本确认有效期。TLS 仍要求已同步的设备时间，不跳过证书校验。

非充电状态下检查至少需要 5% 电量，安装至少 20%；读取电量失败也拒绝准入。主循环在
提交前检查活动更新的电量/联网状态，失效则取消；已进入启动分区提交不能中途反转结果。
阻塞 HTTP 仍有至多配置的请求超时等待，不能据此承诺录音抢占零延迟。

UI 入队前记录取消意图和原检查 ID，旧页面取消不能取消新请求。owner 先发布准备/工作
快照，再确认请求被接受，避免 UI 提前清除 pending 后丢失离页取消的目标。Wi-Fi 设置
改变会等更新实际结束才操作驱动；资源故障或已提交状态不会被普通维护/重启入口绕过。

## 手表页面

`WatchFirmwareSnapshot.available` 为真时，原设置页显示“软件更新”。Application 只在组合模式
初始化后提供此快照；未配置发布公钥/发现地址时显示不可用，不发请求。页面不在进入时自动联网；检查、安装、取消和重启
使用各自的 `WatchViewAction`。网络准备与下载分开显示，未初始化日历不阻止提交更新请求。

“查看更新”进入独立确认页，固定 attempt、sequence、version。后续可用版本改变不会直接
替换确认内容；旧确认禁用，用户须返回重新查看。安装请求携带真实检查请求 ID 和已审阅
版本标识。离开更新流程、隐藏页面会提交取消；离开确认页返回更新页不会误取消整个检查。
尚未收到检查准入结果就离开时，取消仍携带原排队请求 ID，供 owner 拒绝迟到的请求。

提交启动分区后不再发取消。未确认 boot_selected 的进度最高 99%，实际结果决定重启按钮。
所有状态使用中英文资源，错误不显示为“已保存”。页面只是呈现和发请求，不持有清单/密钥、
不写 Flash、不直接重启设备，也不代替控制器的版本验证或资源准入。“安装更新”与完成后的
“重新启动”是两次明确操作；重启须对应已安装版本、同一资源代次且工作器已结束。

`scripts/test_watch_ui.sh` 的中英文桌面 LVGL 测试覆盖状态、过期确认、隐藏/返回取消、迟到
completion、独立重启及底部操作可达性；截图已人工检查。测试使用快照与请求收集器，
不证明物理触摸、真实网络或 Flash 的端到端更新成功。Application 源码接线已经完成，
完整 ESP 目标构建及真机验收仍未执行。

## 验证边界

`scripts/test_ota_upgrade.sh` 使用 ASan/UBSan、真实 libsodium、cJSON/http_parser 及真实 OTA
方法源码；Flash 和网络是测试替身。设置 `SODIUM_SOURCE_DIR` 指向既有主机 libsodium 构建。
设置 `PYTHON_BIN` 指向含 cryptography 的 uv 环境，可额外执行发布工具测试，并将 Python
生成的清单和镜像送入实际 C++ 校验/下载入口。

目标 ESP 固件未构建，故自定义描述在实际 ELF/bin 中的最终位置、硬件镜像验证、分区容量、
真实断网/掉电回退仍待验收。完整资产兼容性/回退、生产公钥配置和发布服务仍是交付阻塞项。
清单没有在线撤销或有效期检查，不能宣称防止签名版本冻结攻击；当前逻辑只拒绝不高于
正在运行镜像序号的更新。签名不能替代发布者对音频、BLE、RTC 和功耗的硬件验收。
