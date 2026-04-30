#!/usr/bin/env bash
set -e
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="$ROOT_DIR/verification/logs"
mkdir -p "$OUT_DIR"
# Find serial device
DEV=$(ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | head -n1 || true)
if [ -z "$DEV" ]; then
  DEV=$(ls /dev/tty.usbmodem* /dev/tty.usbserial* /dev/tty.* 2>/dev/null | head -n1 || true)
fi
if [ -z "$DEV" ]; then
  echo "ERROR: No serial device found" >&2
  exit 2
fi
# set baud for mac/linux
stty -f "$DEV" 115200 2>/dev/null || stty -F "$DEV" 115200 2>/dev/null || true
POST_LOG="$OUT_DIR/serial_postflash_timestamped.log"
rm -f "$POST_LOG"
# Start reader; prefix each line with ISO timestamp
cat "$DEV" | awk '{ cmd = "date +%Y-%m-%dT%H:%M:%S%z"; cmd | getline t; close(cmd); print t " " $0; fflush(); }' > "$POST_LOG" &
CATPID=$!
sleep 0.2
# Send test commands (non-destructive)
for cmd in "clocks" "tick off" "tick on" "blink interval 300" "blink start" "blink stop"; do
  printf "%s\n" "$cmd" > "$DEV" || true
  sleep 0.6
done
sleep 0.6
kill $CATPID 2>/dev/null || true
sleep 0.1
echo "Wrote timestamped post-flash log to: $POST_LOG"
