#!/usr/bin/env bash
# Build TankSync RX firmware for both supported targets.
# Output: build_esp32/tanksync_receiver.bin   (ESP32 DevKit v1)
#         build_esp32s3/tanksync_receiver.bin (ESP32-S3 SuperMini)
#
# Usage:
#   source ~/esp/esp-idf-v5.5.2/export.sh
#   ./scripts/build_all.sh                 # builds both
#   ./scripts/build_all.sh esp32           # builds just ESP32
#   ./scripts/build_all.sh esp32s3         # builds just S3
#
# Per-target build dirs prevent the two targets from trampling each other's
# CMake cache. We never share `build/` between targets — ESP-IDF derives a
# lot of generated headers (sdkconfig.h, etc.) from the active target.

set -euo pipefail

cd "$(dirname "$0")/.."
PROJECT_ROOT="$PWD"

if ! command -v idf.py >/dev/null 2>&1; then
  echo "ERROR: idf.py not on PATH. Run: source ~/esp/esp-idf-v5.5.2/export.sh" >&2
  exit 1
fi

build_one() {
  local target="$1"
  local builddir="build_${target}"
  echo "===== Building for ${target} -> ${builddir}/ ====="
  idf.py -B "${builddir}" set-target "${target}"
  idf.py -B "${builddir}" build
  echo "Done: ${PROJECT_ROOT}/${builddir}/tanksync_receiver.bin"
}

case "${1:-all}" in
  esp32)    build_one esp32 ;;
  esp32s3)  build_one esp32s3 ;;
  all)
    build_one esp32
    build_one esp32s3
    ;;
  *)
    echo "Unknown target: $1 (expected esp32 | esp32s3 | all)" >&2
    exit 1
    ;;
esac
