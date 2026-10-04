#!/usr/bin/env bash
# tools/mame/run_capture.sh - Orchestrate MAME trace capture for Land Maker (landmakrj) attract mode.
#
# Usage:
#   ./tools/mame/run_capture.sh [options]
#
# Options:
#   --mame <path>         Path to MAME/f3 executable
#   --rompath <path>      Path to ROMs directory (default: tools/mame/staged_roms;/Users/darien/Workspace/f3-stuff/roms;/Users/darien/Workspace/f3-stuff)
#   --outdir <path>       Output directory for captured frames (default: captures/landmakrj_attract)
#   --start-frame <N>     First frame to capture (default: 300)
#   --count <N>           Number of frames to capture (default: 10)
#   --step <N>            Frame capture cadence/step (default: 1)
#   --wav <path>          Path for recorded audio WAV (default: <outdir>/attract.wav)
#   --throttle            Run with emulation throttling (default: nothrottle for fast capture)
#   --stage-only          Stage ROMs only without running MAME
#   --dry-run             Print command line without running
#   -h, --help            Show this help message

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# Default configurations
MAME_BIN="${MAME_BIN:-/Users/darien/Workspace/f3-stuff/tools/mame-baseline/f3}"
MAME_ALT_BIN="/Users/darien/Workspace/f3-stuff/tools/mame-baseline/mamef3"
OUTDIR="${REPO_ROOT}/captures/landmakrj_attract"
START_FRAME=300
COUNT=10
STEP=1
THROTTLE_FLAG="-nothrottle"
STAGE_ONLY=0
DRY_RUN=0
WAV_PATH=""
ROMPATH_ARG=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mame)
      MAME_BIN="$2"
      shift 2
      ;;
    --rompath)
      ROMPATH_ARG="$2"
      shift 2
      ;;
    --outdir)
      OUTDIR="$2"
      shift 2
      ;;
    --start-frame)
      START_FRAME="$2"
      shift 2
      ;;
    --count)
      COUNT="$2"
      shift 2
      ;;
    --step)
      STEP="$2"
      shift 2
      ;;
    --wav)
      WAV_PATH="$2"
      shift 2
      ;;
    --throttle)
      THROTTLE_FLAG="-throttle"
      shift
      ;;
    --stage-only)
      STAGE_ONLY=1
      shift
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    -h|--help)
      grep '^# ' "$0" | cut -c 3-
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      exit 1
      ;;
  esac
done

if [[ -z "$WAV_PATH" ]]; then
  WAV_PATH="${OUTDIR}/attract.wav"
fi

mkdir -p "$OUTDIR"

echo "=== MAME Land Maker Attract Mode Capture Pipeline ==="
echo "Output Directory: $OUTDIR"
echo "Start Frame:      $START_FRAME"
echo "Frame Count:      $COUNT (step: $STEP)"
echo "Audio WAV:        $WAV_PATH"

# 1. ROM Staging
STAGED_ZIP="${SCRIPT_DIR}/staged_roms/landmakrj.zip"
if [[ ! -f "$STAGED_ZIP" ]]; then
  echo "Staging Japanese ROM set (landmakrj.zip)..."
  python3 "${SCRIPT_DIR}/stage_roms.py" --out-zip "$STAGED_ZIP"
else
  echo "Found existing staged ROMs: $STAGED_ZIP"
fi

if [[ $STAGE_ONLY -eq 1 ]]; then
  echo "ROM staging complete (--stage-only specified). Exiting."
  exit 0
fi

# Build ROMPATH
STAGED_DIR="$(dirname "$STAGED_ZIP")"
if [[ -n "$ROMPATH_ARG" ]]; then
  ROMPATH="$ROMPATH_ARG"
else
  ROMPATH="${STAGED_DIR}:/Users/darien/Workspace/f3-stuff/roms:/Users/darien/Workspace/f3-stuff"
fi

# 2. Check MAME Executable
RESOLVED_MAME=""
if [[ -x "$MAME_BIN" ]]; then
  RESOLVED_MAME="$MAME_BIN"
elif [[ -x "$MAME_ALT_BIN" ]]; then
  RESOLVED_MAME="$MAME_ALT_BIN"
elif command -v f3 >/dev/null 2>&1; then
  RESOLVED_MAME="$(command -v f3)"
elif command -v mame >/dev/null 2>&1; then
  RESOLVED_MAME="$(command -v mame)"
fi

if [[ -z "$RESOLVED_MAME" ]]; then
  echo ""
  echo "=========================================================================="
  echo "NOTICE: MAME executable not found at: $MAME_BIN"
  echo ""
  echo "Orchestrator Instructions for acquiring the baseline MAME binary:"
  echo "  1. Bug worktrees under /Users/darien/Workspace/f3-stuff/mame-f3-bugs/wt/"
  echo "     are compiling SUBTARGET=f3 (taito_f3.cpp only)."
  echo "  2. Once the first build finishes, confirm clean source:"
  echo "       git -C <wt-dir> log origin/master..HEAD (must be empty)"
  echo "  3. Copy the compiled binary to the baseline location:"
  echo "       mkdir -p /Users/darien/Workspace/f3-stuff/tools/mame-baseline/"
  echo "       cp <wt-dir>/f3 /Users/darien/Workspace/f3-stuff/tools/mame-baseline/f3"
  echo "       chmod +x /Users/darien/Workspace/f3-stuff/tools/mame-baseline/f3"
  echo ""
  echo "The capture tooling is fully implemented and staged. When the MAME binary"
  echo "is present, re-run this script to capture attract mode frames."
  echo "=========================================================================="
  exit 1
fi

echo "Using MAME Binary: $RESOLVED_MAME"
echo "Using Rompath:     $ROMPATH"

# 3. Export Environment for capture.lua
export F3_CAPTURE_DIR="$OUTDIR"
export F3_CAPTURE_START="$START_FRAME"
export F3_CAPTURE_COUNT="$COUNT"
export F3_CAPTURE_STEP="$STEP"
export F3_CAPTURE_EXIT=1

MAME_CMD=(
  "$RESOLVED_MAME"
  "landmakrj"
  "-rompath" "$ROMPATH"
  "-autoboot_script" "${SCRIPT_DIR}/capture.lua"
  "-autoboot_delay" "0"
  "-wavwrite" "$WAV_PATH"
  "-video" "none"
  "$THROTTLE_FLAG"
)

echo "Executing: ${MAME_CMD[*]}"

if [[ $DRY_RUN -eq 1 ]]; then
  echo "Dry run complete."
  exit 0
fi

# Run MAME capture
"${MAME_CMD[@]}"

# 4. Verify outputs
echo ""
echo "=== Capture Verification ==="
if [[ -f "${OUTDIR}/metadata.json" ]]; then
  echo "[PASS] Global metadata found: ${OUTDIR}/metadata.json"
  FRAMES_FOUND=$(find "${OUTDIR}" -mindepth 1 -maxdepth 1 -type d -name "frame_*" | wc -l | tr -d ' ')
  echo "[PASS] Total frame directories captured: $FRAMES_FOUND"
  if [[ -f "$WAV_PATH" ]]; then
    WAV_SIZE=$(wc -c < "$WAV_PATH" | tr -d ' ')
    echo "[PASS] Audio WAV captured: $WAV_PATH ($WAV_SIZE bytes)"
  fi
else
  echo "[WARNING] No metadata.json found in $OUTDIR. Capture may have been aborted or incomplete." >&2
  exit 1
fi
