# 🎉 服务器配置功能已实现！

## ✅ 完成的修改

### 1. 新增文件

| 文件 | 说明 |
|------|------|
| `main/server_config.h` | 服务器配置头文件 |
| `main/server_config.cc` | 服务器配置实现 |
| `scripts/set_server_config.py` | Python 配置工具 |
| `scripts/config_tulingbc.sh` | 快速配置脚本（图灵必驰） |
| `scripts/README_SERVER_CONFIG.md` | 详细配置文档 |

### 2. 修改的文件

| 文件 | 修改内容 |
|------|----------|
| `main/Kconfig.projbuild` | 更新默认 OTA URL 为 audream.tulingbc.com |
| `main/sdkconfig` | 更新当前配置的 OTA URL |
| `main/ota.cc` | 集成 ServerConfig，支持 NVS 配置 |
| `main/protocols/websocket_protocol.cc` | 集成 ServerConfig |
| `main/mcp_server.cc` | 集成 ServerConfig |
| `main/CMakeLists.txt` | 添加 server_config.cc 到构建 |

### 3. 默认服务器地址

```ini
OTA URL        = https://audream.tulingbc.com/xiaozhi/ota/
WebSocket URL = wss://audream.tulingbc.com/xiaozhi/v1/
Vision URL    = https://audream.tulingbc.com/mcp/vision/explain
```

## 🚀 使用方法

### 快速配置（推荐）

```bash
# 使用快速配置脚本设置图灵必驰服务器
./scripts/config_tulingbc.sh /dev/ttyUSB0

# 或使用 Python 工具手动配置
python3 scripts/set_server_config.py --port /dev/ttyUSB0 \
    --ota https://audream.tulingbc.com/xiaozhi/ota/ \
    --websocket wss://audream.tulingbc.com/xiaozhi/v1/ \
    --vision https://audream.tulingbc.com/mcp/vision/explain

# 查看当前配置
python3 scripts/set_server_config.py --port /dev/ttyUSB0 --show

# 清除配置（恢复默认）
python3 scripts/set_server_config.py --port /dev/ttyUSB0 --clear
```

### 配置优先级

1. **NVS 配置** - 用户通过脚本/命令设置的值（最高优先级）
2. **OTA 返回值** - 从 OTA 服务器动态获取的配置
3. **固件默认值** - 编译时设置的默认值（最低优先级）

### 工作流程

```
设备启动
  ↓
检查 NVS 配置
  ↓
如果 NVS 有配置 → 使用 NVS 配置
  ↓
如果 NVS 无配置 → 使用固件默认值连接 OTA
  ↓
OTA 返回 WebSocket 和 Vision 配置
  ↓
保存到 WiFi 命名空间
  ↓
建立连接
```

## 📋 编译和烧录

```bash
# 设置 ESP-IDF 环境（如果需要）
. ~/esp/esp-idf/export.sh

# 编译项目
idf.py build

# 烧录固件
idf.py flash

# 监控日志
idf.py monitor
```

## 🔍 日志示例

**使用 NVS 配置时：**
```
I (1234) ServerConfig: Using NVS OTA URL: https://audream.tulingbc.com/xiaozhi/ota/
I (1235) WS: Using NVS WebSocket URL: wss://audream.tulingbc.com/xiaozhi/v1/
I (1236) ServerConfig: Using NVS Vision URL: https://audream.tulingbc.com/mcp/vision/explain
```

**使用默认配置时：**
```
I (1234) ServerConfig: Using default OTA URL: https://audream.tulingbc.com/xiaozhi/ota/
```

## 🛠️ NVS 结构

配置存储在 `server` 命名空间：

| 键 | 类型 | 值 |
|---|------|-----|
| `ota_url` | string | https://audream.tulingbc.com/xiaozhi/ota/ |
| `websocket_url` | string | wss://audream.tulingbc.com/xiaozhi/v1/ |
| `vision_url` | string | https://audream.tulingbc.com/mcp/vision/explain |

## 📖 详细文档

查看 `scripts/README_SERVER_CONFIG.md` 获取完整的配置指南。

## ⚠️ 注意事项

1. **首次使用**: 需要重新编译并烧录固件
2. **配置生效**: 设置后需要重启设备
3. **配置持久化**: NVS 配置会永久保存，直到被清除
4. **向后兼容**: 旧的 WiFi 命名空间配置仍然有效

## 🎯 下一步

1. 编译并烧录新固件
2. 使用配置脚本设置服务器地址
3. 重启设备验证配置
4. 通过串口日志确认连接到正确的服务器

## 📞 服务器端实现

您的后端服务器需要实现以下接口：

### OTA 接口
```
GET https://audream.tulingbc.com/xiaozhi/ota/
```

响应格式：
```json
{
  "websocket": {
    "url": "wss://audream.tulingbc.com/xiaozhi/v1/",
    "token": "Bearer your-token",
    "version": 1
  },
  "vision": {
    "url": "https://audream.tulingbc.com/mcp/vision/explain",
    "token": "Bearer your-vision-token"
  },
  "server_time": {
    "timestamp": 1709876543000,
    "timezone_offset": 480
  },
  "firmware": {
    "version": "2.2.4",
    "url": "https://audream.tulingbc.com/firmware/xiaozhi.bin"
  }
}
```

### WebSocket 服务
```
wss://audream.tulingbc.com/xiaozhi/v1/
```

### 视觉分析接口
```
POST https://audream.tulingbc.com/mcp/vision/explain
Content-Type: multipart/form-data
```
