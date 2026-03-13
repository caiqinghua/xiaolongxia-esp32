# 服务器配置指南

## 概述

从现在开始，您可以通过 **NVS (Non-Volatile Storage)** 配置后端服务器地址，无需重新编译固件。

## 配置方式

### 方式一：使用配置脚本（推荐）

使用 Python 配置工具来设置服务器 URL：

```bash
# 安装依赖
pip install pyserial

# 设置所有服务器地址
python scripts/set_server_config.py --port /dev/ttyUSB0 \
    --ota https://api.tulingbc.com/xiaozhi/ota/ \
    --websocket wss://api.tulingbc.com/xiaozhi/v1/ \
    --vision https://api.tulingbc.com/mcp/vision/explain

# 仅设置 OTA URL
python scripts/set_server_config.py --port /dev/ttyUSB0 \
    --ota https://api.tulingbc.com/xiaozhi/ota/

# 查看当前配置
python scripts/set_server_config.py --port /dev/ttyUSB0 --show

# 清除所有配置（恢复固件默认值）
python scripts/set_server_config.py --port /dev/ttyUSB0 --clear

# 列出可用串口
python scripts/set_server_config.py --list
```

### 方式二：通过串口命令

如果您的设备支持串口命令，可以直接发送 JSON 命令：

**设置 OTA URL：**
```json
{"action":"set_server_config","ota_url":"https://api.tulingbc.com/xiaozhi/ota/"}
```

**设置 WebSocket URL：**
```json
{"action":"set_server_config","websocket_url":"wss://api.tulingbc.com/xiaozhi/v1/"}
```

**设置 Vision URL：**
```json
{"action":"set_server_config","vision_url":"https://api.tulingbc.com/mcp/vision/explain"}
```

**清除所有配置：**
```json
{"action":"clear_server_config"}
```

**查看配置：**
```json
{"action":"get_server_config"}
```

### 方式三：修改默认值（需重新编译）

如果需要修改固件的默认服务器地址，编辑以下文件：

1. **main/Kconfig.projbuild** - 修改默认 OTA URL
2. **重新编译固件**
   ```bash
   idf.py build flash
   ```

## 配置优先级

系统按以下优先级查找配置：

1. **NVS 配置** (server namespace) - 最高优先级
2. **WiFi 命名空间配置** (向后兼容)
3. **固件默认值** (CONFIG_OTA_URL) - 最低优先级

## NVS 存储结构

服务器配置存储在 `server` 命名空间：

| 键名 | 类型 | 说明 |
|------|------|------|
| `ota_url` | string | OTA 检查和配置服务器地址 |
| `websocket_url` | string | WebSocket 服务器地址 |
| `vision_url` | string | 视觉分析服务器地址 |

## 默认配置

固件默认配置为：

```ini
OTA URL        = https://api.tulingbc.com/xiaozhi/ota/
WebSocket URL = (从 OTA 服务器获取)
Vision URL    = (从 OTA 服务器获取)
```

## API 接口

设备启动时会连接到 OTA 服务器获取最新配置，服务器需要返回以下 JSON 格式：

```json
{
  "websocket": {
    "url": "wss://api.tulingbc.com/xiaozhi/v1/",
    "token": "Bearer your-token-here",
    "version": 1
  },
  "vision": {
    "url": "https://api.tulingbc.com/mcp/vision/explain",
    "token": "Bearer your-vision-token-here"
  },
  "server_time": {
    "timestamp": 1709876543000,
    "timezone_offset": 480
  },
  "firmware": {
    "version": "2.2.4",
    "url": "https://api.tulingbc.com/firmware/xiaozhi.bin"
  }
}
```

## 故障排除

### 配置未生效

1. 检查 NVS 是否成功写入
2. 重启设备使配置生效
3. 查看串口日志确认配置加载情况

### 恢复默认配置

```bash
# 清除所有 NVS 服务器配置
python scripts/set_server_config.py --port /dev/ttyUSB0 --clear

# 重启设备
idf.py reset
```

### 查看配置日志

设备启动时会显示当前使用的配置：

```
I (1234) ServerConfig: Using NVS OTA URL: https://api.tulingbc.com/xiaozhi/ota/
I (1235) ServerConfig: Using NVS WebSocket URL: wss://api.tulingbc.com/xiaozhi/v1/
I (1236) ServerConfig: Using NVS Vision URL: https://api.tulingbc.com/mcp/vision/explain
```

如果使用默认值，会显示：

```
I (1234) ServerConfig: Using default OTA URL: https://api.tulingbc.com/xiaozhi/ota/
```

## 技术细节

### ServerConfig 类

`main/server_config.cc` 实现了服务器配置管理：

- `GetOtaUrl()` - 获取 OTA URL
- `GetWebSocketUrl()` - 获取 WebSocket URL
- `GetVisionUrl()` - 获取 Vision URL
- `SetOtaUrl(url)` - 设置 OTA URL 到 NVS
- `SetWebSocketUrl(url)` - 设置 WebSocket URL 到 NVS
- `SetVisionUrl(url)` - 设置 Vision URL 到 NVS
- `ClearAll()` - 清除所有配置

### 集成位置

- `ota.cc` - OTA URL 获取
- `websocket_protocol.cc` - WebSocket URL 获取
- `mcp_server.cc` - Vision URL 获取
