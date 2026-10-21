#!/bin/sh
# Builds an SD card app and signs it with the project's key.
#
#   tools/build_app.sh apps/analog_clock            # -> build/apps/analog_clock.app
#   tools/build_app.sh apps/analog_clock --unsigned # for developer mode
#
# Then copy it to the display:
#   ~/.espressif/python_env/idf5.5_py3.14_env/bin/python tools/sdcard.py put build/apps/analog_clock.app apps/
# and restart the display.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$(cd "$1" && pwd)"
NAME="$(basename "$APP")"
shift
OUT="$ROOT/build/apps"
mkdir -p "$OUT"

. "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1
# Only the app's ELF file (the "elf" target), not a whole firmware image.
{ idf.py -C "$APP" -B "$OUT/$NAME" -DIDF_TARGET=esp32s3 reconfigure &&
  cmake --build "$OUT/$NAME" --target elf; } >"$OUT/$NAME.log" 2>&1 || {
    grep -E "error|Error" "$OUT/$NAME.log" | head -20
    echo "Build failed; full log: $OUT/$NAME.log"
    exit 1
}
# The ELF is named after the project() in the app's CMakeLists.txt.
ELF="$(ls "$OUT/$NAME"/*.app.elf | head -1)"
python "$ROOT/tools/appkey.py" sign "$ELF" "$APP/app.json" \
    -o "$OUT/$NAME.app" "$@"
