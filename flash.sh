#!/usr/bin/env bash
# AEROCyd flasher for Debian/Ubuntu: installs PlatformIO + esptool, builds and flashes the CYD.
# Usage: ./flash.sh [--erase] [--monitor] [--port /dev/ttyUSB0]
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

ERASE=0; MONITOR=0; PORT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --erase) ERASE=1;; --monitor) MONITOR=1;;
    --port) PORT="${2:-}"; shift;;
    -h|--help) sed -n '2,3p' "$0"; exit 0;;
    *) echo "unknown option: $1"; exit 1;;
  esac; shift
done

say(){ printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
die(){ printf '\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

# ---- 1. OS check ----
[ -r /etc/os-release ] || die "cannot detect OS"
. /etc/os-release
case "${ID:-}:${ID_LIKE:-}" in
  *debian*|*ubuntu*) say "Detected ${PRETTY_NAME:-Debian-based system}";;
  *) die "This script supports Debian/Ubuntu only (found: ${PRETTY_NAME:-unknown})";;
esac
SUDO=""; [ "$(id -u)" -ne 0 ] && { command -v sudo >/dev/null || die "sudo is required"; SUDO="sudo"; }

# ---- 2. system packages ----
say "Installing system packages (python3, venv, pip, git, curl)"
$SUDO apt-get update -qq
$SUDO apt-get install -y python3 python3-venv python3-pip git curl ca-certificates

# ---- 3. venv with PlatformIO + esptool ----
if [ ! -x .venv/bin/pio ]; then
  say "Creating virtualenv and installing PlatformIO + esptool"
  python3 -m venv .venv
  .venv/bin/pip install --upgrade pip wheel
  .venv/bin/pip install platformio esptool pyserial
fi
export PATH="$PWD/.venv/bin:$PATH"
export PLATFORMIO_CORE_DIR="$PWD/.pio-core"   # keep everything inside this folder

# ---- 4. find the board ----
find_port(){ ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | head -n1 || true; }
if [ -z "$PORT" ]; then
  PORT="$(find_port)"
  if [ -z "$PORT" ] && dpkg -s brltty >/dev/null 2>&1; then
    say "brltty is installed and steals CH340/CP210x serial ports - removing it"
    $SUDO apt-get remove -y brltty
    echo "Unplug and replug the CYD, then press Enter..."; read -r _ </dev/tty || true
    PORT="$(find_port)"
  fi
fi
[ -n "$PORT" ] || die "No serial port found. Plug the CYD in with a DATA USB cable and re-run (or use --port)."
say "Using port $PORT"

if ! [ -w "$PORT" ]; then
  say "No write access to $PORT - adding $USER to dialout and opening the port for this session"
  $SUDO usermod -aG dialout "$USER" || true
  $SUDO chmod a+rw "$PORT"
fi

# ---- 5. optional full erase ----
if [ "$ERASE" -eq 1 ]; then
  say "Erasing flash"
  esptool.py --port "$PORT" erase_flash
fi

# ---- 6. build + upload (retry with slower baud rates) ----
OK=0
for SPEED in 921600 460800 115200; do
  say "Building and uploading at $SPEED baud"
  sed -i "s/^upload_speed.*/upload_speed = $SPEED/" platformio.ini
  if pio run -e cyd -t upload --upload-port "$PORT"; then OK=1; break; fi
  echo "Upload failed at $SPEED - hold BOOT on the board while it says 'Connecting...' if needed. Retrying slower."
done
sed -i "s/^upload_speed.*/upload_speed = 921600/" platformio.ini
[ "$OK" -eq 1 ] || die "Flashing failed. Try: ./flash.sh --erase, another cable/port, or hold BOOT during upload."

say "Done! AEROCyd is flashed. The board will reboot into the boot animation."
if [ "$MONITOR" -eq 1 ]; then pio device monitor -p "$PORT" -b 115200; fi
