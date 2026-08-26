#!/usr/bin/env bash
# Full clean-start benchmark: build from scratch, run in Wokwi, save the log.
#
#   export WOKWI_CLI_TOKEN=wok_...        # from https://wokwi.com/dashboard/ci
#   ./run_benchmark.sh
set -euo pipefail
cd "$(dirname "$0")"

: "${WOKWI_CLI_TOKEN:?set WOKWI_CLI_TOKEN (get one at https://wokwi.com/dashboard/ci)}"
: "${IDF_PATH:=$HOME/esp/esp-idf}"
# shellcheck disable=SC1091
source "$IDF_PATH/export.sh" >/dev/null 2>&1

echo "== clean =="
rm -rf build sdkconfig

echo "== build (esp32s3) =="
idf.py set-target esp32s3 >/dev/null
idf.py build >/dev/null
echo "   app: $(stat -f%z build/kws.bin 2>/dev/null || stat -c%s build/kws.bin) bytes"

echo "== merged image for wokwi.com upload =="
( cd build && python -m esptool --chip esp32s3 merge_bin -o wokwi-upload.bin @flash_args >/dev/null )
echo "   build/wokwi-upload.bin"

echo "== simulate =="
LOG=benchmark-$(date +%Y%m%d-%H%M%S).log
wokwi-cli --timeout 600000 --expect-text PHASE12_OK --serial-log-file "$LOG" .
echo
echo "full log: $LOG"
