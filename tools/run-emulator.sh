#!/usr/bin/env bash
# Emulate a boot image and render its screen. Usage: tools/run-emulator.sh [boot.img] [out.png] [--load ADDR]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export LIBUNICORN_PATH="$ROOT/upstream/emu/unicorn/build"
export PYTHONPATH="$ROOT/upstream/emu/unicorn/bindings/python"
IMG="${1:-$ROOT/out/a5y17lte-step1b-boot.img}"; PNG="${2:-$ROOT/out/step1b-screen.png}"
shift $(( $# > 2 ? 2 : $# ))
exec python3 "$ROOT/tools/emulate_step1.py" "$IMG" "$PNG" "$@"
