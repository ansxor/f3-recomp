#!/usr/bin/env bash
# Runs the New 3DS frame-budget bench in Azahar under Xvfb and prints the per-scenario gate table.
#
#   tools/bench/n3ds-bench.sh CLOCK TAG [--no-build]
#
# CLOCK is Azahar's cpu_clock_percentage (100 = the 268 MHz ARM11 gate, 300 = the 804 MHz New 3DS
# speedup clock). TAG names the output directory /tmp/bench/TAG, which receives f3rt.log, one
# screenshot per scenario (taken half way through it), report.txt and azahar.log. Run from the
# repository root with nix, Xvfb and xdotool on PATH; nothing else should load the machine.
#
# It builds the bench 3dsx (`nix run .#3ds-bench`, build/landmakrj-3ds-bench), installs
# games/landmakrj/n3ds/bench.txt on the emulated SD card, edits ~/.config/azahar-emu/qt-config.ini
# for the run and restores it afterwards. The run ends at the script's `end` marker (about 5.5
# minutes of emulated time, longer when the emulated console runs slower than real time).
# Exit status is that of the gate: 0 pass, 1 fail.
set -euo pipefail

usage() {
    sed -n '2,/^set -/p' "$0" | sed '$d; s/^# \{0,1\}//' >&2
    exit 2
}
[ "$#" -ge 2 ] || usage
clock=$1
tag=$2
shift 2
build=1
for arg in "$@"; do
    case $arg in
        --no-build) build=0 ;;
        *) usage ;;
    esac
done
case $clock in
    [0-9] | [0-9][0-9] | [0-9][0-9][0-9]) ;;
    *) printf 'error: CLOCK must be a percentage (100 or 300), got %s\n' "$clock" >&2; exit 2 ;;
esac
[ -f flake.nix ] && [ -f games/landmakrj/n3ds/bench.txt ] || { echo 'error: run from the repository root' >&2; exit 1; }
for tool in nix Xvfb xdotool python3; do
    command -v "$tool" >/dev/null || { printf 'error: %s is not on PATH\n' "$tool" >&2; exit 1; }
done

game=landmakrj
sdmc=$HOME/.local/share/azahar-emu/sdmc
data=$sdmc/3ds/f3-recomp/$game
config=$HOME/.config/azahar-emu/qt-config.ini
shots=$HOME/.local/share/azahar-emu/screenshots
out=/tmp/bench/$tag
three_dsx=build/$game-3ds-bench/$game.3dsx
# Longest a run may take, in seconds, and how long after a scenario marker its screenshot is taken.
limit=1800
shot_delay=35
scenarios=(attract menu charselect gameplay)

[ "$build" = 0 ] || nix run .#3ds-bench
[ -f "$three_dsx" ] || { printf 'error: %s is missing (run without --no-build)\n' "$three_dsx" >&2; exit 1; }
azahar=$(nix build --no-link --print-out-paths --inputs-from . nixpkgs#azahar)/bin/azahar
[ -x "$azahar" ] || { printf 'error: no azahar at %s\n' "$azahar" >&2; exit 1; }
pgrep -x azahar >/dev/null && { echo 'error: azahar is already running' >&2; exit 1; }

mkdir -p "$out" "$data" "$shots"
rm -f "$out"/*
[ -f "$config" ] || { printf 'error: %s is missing; start Azahar once to create it\n' "$config" >&2; exit 1; }
config_backup=$(mktemp)
cp "$config" "$config_backup"
azahar_pid=
xvfb_pid=
cleanup() {
    [ -z "$azahar_pid" ] || kill -KILL "$azahar_pid" 2>/dev/null || true
    [ -z "$xvfb_pid" ] || kill "$xvfb_pid" 2>/dev/null || true
    cp "$config_backup" "$config"
    rm -f "$config_backup"
}
trap cleanup EXIT

# cpu_clock_percentage lives in [Core]; the key and its \default marker are rewritten together.
sed -i -E "s/^cpu_clock_percentage=.*/cpu_clock_percentage=$clock/; s/^cpu_clock_percentage\\\\default=.*/cpu_clock_percentage\\\\default=false/" "$config"
grep -q "^cpu_clock_percentage=$clock\$" "$config" || { echo 'error: cpu_clock_percentage not found in qt-config.ini' >&2; exit 1; }

install -m 644 games/$game/n3ds/bench.txt "$data/bench.txt"
rm -f "$data/f3rt.log"
display=:$((100 + $$ % 800))
Xvfb "$display" -screen 0 1024x768x24 >"$out/xvfb.log" 2>&1 &
xvfb_pid=$!
export DISPLAY=$display
sleep 2
"$azahar" "$three_dsx" >"$out/azahar.log" 2>&1 &
azahar_pid=$!
printf 'bench %s at cpu_clock_percentage=%s, azahar pid %s, display %s\n' "$tag" "$clock" "$azahar_pid" "$display"

# Capture Azahar's own screenshot (Ctrl+P, both screens) and keep the newest file as NAME.png.
screenshot() {
    local before after window
    before=$(find "$shots" -name '*.png' | wc -l)
    window=$(xdotool search --onlyvisible --name Azahar | tail -n 1 || true)
    [ -z "$window" ] || xdotool windowfocus --sync "$window" || true
    xdotool key --clearmodifiers ctrl+p
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        sleep 1
        after=$(find "$shots" -name '*.png' | wc -l)
        [ "$after" -gt "$before" ] && break
    done
    if [ "$after" -gt "$before" ]; then
        cp "$(find "$shots" -name '*.png' -printf '%T@ %p\n' | sort -n | tail -n 1 | cut -d' ' -f2-)" "$out/$1.png"
        printf 'screenshot %s\n' "$1"
    else
        printf 'warning: no screenshot for %s\n' "$1" >&2
    fi
}

log=$data/f3rt.log
started=$SECONDS
next=0
shot_at=
while kill -0 "$azahar_pid" 2>/dev/null; do
    [ $((SECONDS - started)) -lt "$limit" ] || { echo 'error: bench timed out' >&2; break; }
    if [ -f "$log" ]; then
        grep -q '^n3ds: scenario end ' "$log" && break
        grep -q 'session error' "$log" && { echo 'error: the 3dsx reported a session error:' >&2; grep 'session error' "$log" >&2; break; }
        if [ "$next" -lt "${#scenarios[@]}" ] && grep -q "^n3ds: scenario ${scenarios[$next]} " "$log"; then
            shot_at=$((SECONDS + shot_delay))
            shot_name=${scenarios[$next]}
            next=$((next + 1))
        fi
    fi
    if [ -n "$shot_at" ] && [ "$SECONDS" -ge "$shot_at" ]; then
        screenshot "$shot_name"
        shot_at=
    fi
    sleep 1
done
cp "$log" "$out/f3rt.log"
# SIGTERM leaves Azahar hanging in its crash handler under Xvfb.
kill -KILL "$azahar_pid" 2>/dev/null || true
wait "$azahar_pid" 2>/dev/null || true
azahar_pid=

status=0
python3 -m tools.bench.n3ds_report "$out/f3rt.log" | tee "$out/report.txt" || status=$?
printf 'results in %s\n' "$out"
exit "$status"
