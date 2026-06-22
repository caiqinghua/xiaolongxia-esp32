#!/bin/bash
# 快速配置脚本 - 设置为图灵必驰服务器
# Usage: ./scripts/config_tulingbc.sh [serial_port]

SERIAL_PORT=${1:-"/dev/ttyUSB0"}
OTA_URL="https://audream.tulingbc.com/xiaozhi/ota/"
WEBSOCKET_URL="wss://audream.tulingbc.com/xiaozhi/v1/"
VISION_URL="https://audream.tulingbc.com/mcp/vision/explain"

echo "🚀 配置图灵必驰服务器..."
echo "串口: $SERIAL_PORT"
echo ""

# 检查 Python 脚本是否存在
if [ ! -f "scripts/set_server_config.py" ]; then
    echo "❌ 找不到配置脚本: scripts/set_server_config.py"
    exit 1
fi

# 执行配置
python3 scripts/set_server_config.py \
    --port "$SERIAL_PORT" \
    --ota "$OTA_URL" \
    --websocket "$WEBSOCKET_URL" \
    --vision "$VISION_URL"

if [ $? -eq 0 ]; then
    echo ""
    echo "✅ 配置完成！"
    echo "⚠️  请重启设备使配置生效"
else
    echo ""
    echo "❌ 配置失败"
    exit 1
fi
