#!/bin/bash
# Source this file to activate the ESP-IDF environment:
#   . ./activate.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export IDF_TOOLS_PATH="$SCRIPT_DIR/esp-idf-tools"
source "$SCRIPT_DIR/esp-idf/export.sh"

echo "ESP-IDF environment ready. Target: ESP32-S3"
echo "Port: /dev/ttyACM0  (ESP32-S3-EYE built-in USB Serial/JTAG)"
echo ""
echo "Common commands:"
echo "  idf.py build                   - build the project"
echo "  idf.py flash -p /dev/ttyACM0   - flash to board"
echo "  idf.py monitor -p /dev/ttyACM0 - open serial monitor"
echo "  idf.py flash monitor -p /dev/ttyACM0  - flash + monitor"
