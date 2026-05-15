#!/bin/zsh
set -e

TARGET="${1:-pill_dispenser}"
MONITOR="${2:-}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

find_usbmodem_by_product_id() {
    local product_id="$1"

    ioreg -l -w 0 | awk -v target="$product_id" '
        /AppleUSBACMData/ {
            product = ""
            suffix = ""
            in_block = 1
        }
        in_block && /"idProduct" =/ {
            product = $NF
        }
        in_block && /"IOTTYSuffix" =/ {
            suffix = $NF
            gsub(/"/, "", suffix)
            if (product == target) {
                print "/dev/cu.usbmodem" suffix
                exit
            }
        }
    '
}

if [[ "$MONITOR" == "--monitor" && ! -t 0 && -z "$RUN_PICO_TERMINAL" ]]; then
    echo "CLion started this without an interactive terminal."
    echo "Opening an interactive serial monitor window from the Play button..."

    osascript - "$SCRIPT_DIR" "$TARGET" <<'APPLESCRIPT'
on run argv
    set projectDir to item 1 of argv
    set targetName to item 2 of argv
    set commandText to "cd " & quoted form of projectDir & "; RUN_PICO_TERMINAL=1 ./run-pico.sh " & quoted form of targetName & " --monitor"

    tell application "Terminal"
        activate
        do script commandText
    end tell
end run
APPLESCRIPT

    exit 0
fi

if [[ "$MONITOR" == "--monitor" ]]; then
    FLASH_PICO_RESET=halt "$SCRIPT_DIR/flash-pico.sh" "$TARGET"
else
    "$SCRIPT_DIR/flash-pico.sh" "$TARGET"
fi

if [[ "$MONITOR" != "--monitor" ]]; then
    exit 0
fi

# Close stale monitors from earlier CLion runs before opening the serial port.
pkill -f "$SCRIPT_DIR/tools/serial-monitor.py" 2>/dev/null || true

if [[ -n "$PICO_SERIAL_PORT" ]]; then
    PORT="$PICO_SERIAL_PORT"
else
    # The project prints through the Raspberry Pi Debug Probe serial port.
    # The numeric usbmodem suffix can change, so select the Debug Probe by USB
    # product ID instead of guessing from /dev/cu.usbmodem* ordering.
    PORT="$(find_usbmodem_by_product_id 12)"
    [[ -n "$PORT" ]] || PORT="$(ls /dev/cu.usbmodem* 2>/dev/null | tail -n 1)"
fi

if [[ -z "$PORT" ]]; then
    echo "No /dev/cu.usbmodem* serial port found"
    exit 1
fi

echo "Opening serial monitor on $PORT at 115200 baud"
echo "Stop with the red square in CLion or Ctrl-C."

# The flash step left the Pico halted, so this is the only run reset and the
# startup prompt appears once.
(
    sleep 1
    openocd -s /opt/homebrew/share/openocd/scripts \
    -f "$HOME/pico/rp2040-core0-debug.cfg" \
    -c "init" \
    -c "reset run" \
    -c "shutdown" >/dev/null 2>&1 || true
) &

/usr/bin/python3 "$SCRIPT_DIR/tools/serial-monitor.py" "$PORT" 115200
