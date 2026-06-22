#!/usr/bin/env python3
"""
Server Configuration Tool for Xiaozhi

This script allows you to configure server URLs without recompiling the firmware.
URLs are stored in NVS (Non-Volatile Storage) and persist across reboots.

Usage:
    python set_server_config.py --port <serial_port> --ota <url> --websocket <url> --vision <url>
    python set_server_config.py --port /dev/ttyUSB0 --ota https://audream.tulingbc.com/xiaozhi/ota/
    python set_server_config.py --clear

Examples:
    # Set all URLs to audream.tulingbc.com
    python set_server_config.py --port /dev/ttyUSB0 \\
        --ota https://audream.tulingbc.com/xiaozhi/ota/ \\
        --websocket wss://audream.tulingbc.com/xiaozhi/v1/ \\
        --vision https://audream.tulingbc.com/mcp/vision/explain

    # Clear all NVS server URLs (use defaults from firmware)
    python set_server_config.py --port /dev/ttyUSB0 --clear

    # Show current configuration
    python set_server_config.py --port /dev/ttyUSB0 --show
"""

import argparse
import sys
import json
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("Error: pyserial is required. Install with: pip install pyserial")
    sys.exit(1)


class ServerConfigTool:
    def __init__(self, port, baudrate=115200, timeout=5):
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.serial = None

    def connect(self):
        """Connect to the serial port"""
        try:
            self.serial = serial.Serial(
                port=self.port,
                baudrate=self.baudrate,
                timeout=self.timeout
            )
            print(f"✅ Connected to {self.port}")
            return True
        except serial.SerialException as e:
            print(f"❌ Failed to connect to {self.port}: {e}")
            return False

    def disconnect(self):
        """Disconnect from the serial port"""
        if self.serial and self.serial.is_open:
            self.serial.close()
            print(f"📡 Disconnected from {self.port}")

    def send_command(self, command):
        """Send a command to the device and get response"""
        if not self.serial or not self.serial.is_open:
            print("❌ Not connected to device")
            return None

        # Clear any pending data
        self.serial.reset_input_buffer()
        self.serial.reset_output_buffer()

        # Send command
        cmd_str = json.dumps(command) + "\n"
        self.serial.write(cmd_str.encode())
        print(f"📤 Sent: {cmd_str.strip()}")

        # Wait for response
        start_time = time.time()
        response = b""
        while time.time() - start_time < self.timeout:
            if self.serial.in_waiting > 0:
                response += self.serial.read(self.serial.in_waiting)
                try:
                    # Try to parse as JSON
                    response_str = response.decode().strip()
                    if response_str:
                        return json.loads(response_str)
                except json.JSONDecodeError:
                    pass  # Wait for more data
            time.sleep(0.1)

        print("⏱️ Timeout waiting for response")
        return None

    def set_ota_url(self, url):
        """Set OTA URL in NVS"""
        command = {
            "action": "set_server_config",
            "ota_url": url
        }
        return self.send_command(command)

    def set_websocket_url(self, url):
        """Set WebSocket URL in NVS"""
        command = {
            "action": "set_server_config",
            "websocket_url": url
        }
        return self.send_command(command)

    def set_vision_url(self, url):
        """Set Vision URL in NVS"""
        command = {
            "action": "set_server_config",
            "vision_url": url
        }
        return self.send_command(command)

    def clear_all(self):
        """Clear all server URL overrides"""
        command = {
            "action": "clear_server_config"
        }
        return self.send_command(command)

    def show_config(self):
        """Show current server configuration"""
        command = {
            "action": "get_server_config"
        }
        return self.send_command(command)


def list_serial_ports():
    """List available serial ports"""
    print("🔍 Available serial ports:")
    ports = list_ports.comports()
    for port in ports:
        print(f"  - {port.device}: {port.description}")
    return [port.device for port in ports]


def main():
    parser = argparse.ArgumentParser(
        description="Configure server URLs for Xiaozhi device",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )

    parser.add_argument("--port", "-p", help="Serial port (e.g., /dev/ttyUSB0)")
    parser.add_argument("--list", action="store_true", help="List available serial ports")
    parser.add_argument("--ota", help="Set OTA URL")
    parser.add_argument("--websocket", "-ws", help="Set WebSocket URL")
    parser.add_argument("--vision", help="Set Vision URL")
    parser.add_argument("--clear", action="store_true", help="Clear all server URL overrides")
    parser.add_argument("--show", action="store_true", help="Show current configuration")

    args = parser.parse_args()

    # List ports if requested
    if args.list:
        list_serial_ports()
        return

    # Auto-detect port if not specified
    port = args.port
    if not port:
        ports = [p.device for p in list_ports.comports()]
        if len(ports) == 1:
            port = ports[0]
            print(f"🎯 Auto-detected port: {port}")
        elif len(ports) > 1:
            print("❌ Multiple serial ports found. Please specify one with --port")
            list_serial_ports()
            return
        else:
            print("❌ No serial ports found. Please connect your device.")
            return

    # Create tool instance
    tool = ServerConfigTool(port)

    if not tool.connect():
        return

    try:
        if args.show:
            print("\n📋 Getting current configuration...")
            response = tool.show_config()
            if response:
                print(json.dumps(response, indent=2))
        elif args.clear:
            print("\n🗑️  Clearing all server URL overrides...")
            response = tool.clear_all()
            if response and response.get("status") == "success":
                print("✅ Server configuration cleared")
            else:
                print("❌ Failed to clear configuration")
        else:
            # Set URLs
            urls_set = []
            if args.ota:
                print(f"\n🔧 Setting OTA URL: {args.ota}")
                response = tool.set_ota_url(args.ota)
                if response and response.get("status") == "success":
                    print("✅ OTA URL set successfully")
                    urls_set.append("OTA")
                else:
                    print("❌ Failed to set OTA URL")

            if args.websocket:
                print(f"\n🔧 Setting WebSocket URL: {args.websocket}")
                response = tool.set_websocket_url(args.websocket)
                if response and response.get("status") == "success":
                    print("✅ WebSocket URL set successfully")
                    urls_set.append("WebSocket")
                else:
                    print("❌ Failed to set WebSocket URL")

            if args.vision:
                print(f"\n🔧 Setting Vision URL: {args.vision}")
                response = tool.set_vision_url(args.vision)
                if response and response.get("status") == "success":
                    print("✅ Vision URL set successfully")
                    urls_set.append("Vision")
                else:
                    print("❌ Failed to set Vision URL")

            if urls_set:
                print(f"\n🎉 Configuration updated: {', '.join(urls_set)}")
                print("⚠️  Please reboot the device for changes to take effect")
    finally:
        tool.disconnect()


if __name__ == "__main__":
    main()
