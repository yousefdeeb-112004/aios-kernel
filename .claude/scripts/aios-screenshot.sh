#!/usr/bin/env bash
# Boot the AIOS kernel headless, optionally type a shell command, and capture the
# VGA screen as a PNG. The shell runs on VGA (not serial), so this is the only way
# to observe shell output programmatically.
#
# Usage:
#   .claude/scripts/aios-screenshot.sh [OUT.png] [COMMAND]
#     OUT.png   output image path        (default /tmp/aios-screen.png)
#     COMMAND   shell command to type    (default: none — just the boot screen)
#               only lowercase a-z, 0-9 and spaces are supported by the key sender
# Examples:
#   .claude/scripts/aios-screenshot.sh                 # boot screen
#   .claude/scripts/aios-screenshot.sh /tmp/nz.png nz  # run `nz`, capture result
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-/tmp/aios-screen.png}"
CMD="${2:-}"
PORT=4455
PPM="$(mktemp /tmp/aios-shot.XXXXXX.ppm)"
SER="$(mktemp /tmp/aios-ser.XXXXXX.log)"

cd "$ROOT"
[ -f build/aios-kernel.bin ] || { echo "build/aios-kernel.bin missing — run 'make' first"; exit 1; }
[ -f disk.img ] || dd if=/dev/zero of=disk.img bs=1M count=1 2>/dev/null

qemu-system-i386 -kernel build/aios-kernel.bin -m 128M \
  -no-reboot -no-shutdown -serial file:"$SER" -display none \
  -monitor tcp:127.0.0.1:$PORT,server,nowait \
  -drive file=disk.img,format=raw \
  -device rtl8139,netdev=net0 -netdev user,id=net0 -smp 4 &
QPID=$!
trap 'kill -9 $QPID 2>/dev/null' EXIT

# wait for boot (last init line) or 15s
for _ in $(seq 1 30); do
  grep -q "Device layer + IPC" "$SER" 2>/dev/null && break
  read -t 0.5 < /dev/zero 2>/dev/null || true
done

mon() { printf '%s\n' "$1" | nc -q1 127.0.0.1 $PORT >/dev/null 2>&1; }

if [ -n "$CMD" ]; then
  for ((i=0; i<${#CMD}; i++)); do
    ch="${CMD:$i:1}"
    case "$ch" in
      " ") mon "sendkey spc" ;;
      [a-z0-9]) mon "sendkey $ch" ;;
      *) echo "warn: unsupported key '$ch' skipped" >&2 ;;
    esac
  done
  mon "sendkey ret"
  for _ in 1 2 3; do read -t 0.5 < /dev/zero 2>/dev/null || true; done
fi

mon "screendump $PPM"
for _ in 1 2; do read -t 0.5 < /dev/zero 2>/dev/null || true; done

python3 "$ROOT/.claude/scripts/ppm2png.py" "$PPM" "$OUT"
rm -f "$PPM" "$SER"
echo "screen -> $OUT"
