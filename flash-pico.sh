#!/bin/zsh
set -e

TARGET="${1:-pill_dispenser}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/cmake-build-debug"
ELF="$BUILD_DIR/${TARGET}.elf"
FLASH_PICO_RESET="${FLASH_PICO_RESET:-run}"

if [[ ! -f "$BUILD_DIR/Makefile" && ! -f "$BUILD_DIR/build.ninja" ]]; then
    cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR"
fi

cmake --build "$BUILD_DIR" --target "$TARGET" -j 6

if [[ ! -f "$ELF" ]]; then
    echo "Missing $ELF"
    echo "Build it first with: cmake --build cmake-build-debug --target $TARGET -j 6"
    exit 1
fi

pkill -f openocd 2>/dev/null || true

case "$FLASH_PICO_RESET" in
    run)
        FINAL_RESET_COMMAND="reset run"
        ;;
    halt)
        FINAL_RESET_COMMAND="reset halt"
        ;;
    *)
        echo "FLASH_PICO_RESET must be 'run' or 'halt'"
        exit 1
        ;;
esac

openocd -s /opt/homebrew/share/openocd/scripts \
-f "$HOME/pico/rp2040-core0-debug.cfg" \
-c "init" \
-c "reset halt" \
-c "program $ELF verify" \
-c "$FINAL_RESET_COMMAND" \
-c "shutdown"
