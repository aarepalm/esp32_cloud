#!/bin/bash
# Source this file to activate the ESP-IDF environment:
#   . ./activate.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export IDF_TOOLS_PATH="$SCRIPT_DIR/esp-idf-tools"

# On macOS, system python3 is 3.9 which has importlib.metadata bugs with namespace packages.
# If python3.11 is available (e.g. via Homebrew), shim python3 -> python3.11 so that
# detect_python.sh and activate.py use the correct version and find the right venv.
if [ -x "/opt/homebrew/bin/python3.11" ]; then
    mkdir -p "$SCRIPT_DIR/.python-shim"
    ln -sf /opt/homebrew/bin/python3.11 "$SCRIPT_DIR/.python-shim/python3"
    export PATH="$SCRIPT_DIR/.python-shim:$PATH"
fi

source "$SCRIPT_DIR/esp-idf/export.sh"
if ! command -v idf.py &>/dev/null; then
    echo "ERROR: ESP-IDF activation failed. Run: cd esp-idf && ./install.sh esp32s3 && cd .."
    return 1
fi

echo "ESP-IDF environment ready. Target: ESP32-S3"
echo "Port: Linux: /dev/ttyACM0  macOS: /dev/cu.usbmodem<id>  (ESP32-S3-EYE built-in USB Serial/JTAG)"
echo ""
echo "Common commands:"
echo "  idf.py build                 - build the project"
echo "  idf.py flash -p <port>       - flash to board"
echo "  idf.py monitor -p <port>     - open serial monitor"
echo "  idf.py flash monitor -p <port>  - flash + monitor"
