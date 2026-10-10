#!/usr/bin/env bash
# tools/mac_bench.sh NAME [ENV=VALUE...]: macOS counterpart of tools/bench.sh. Starts the game
# with frame stats and a pad file, enters the level from the title (cross, cross), stands still
# for ${HOLD:-40} s, closes the game and prints a summary of the in-level frame stats.
# Log: out/bench_NAME_<time>.log, per-frame CSV: out/bench_NAME_<time>.frames.csv.
set -u
cd -- "$(dirname -- "$0")/.."
name=${1:?usage: tools/mac_bench.sh NAME [ENV=VALUE...]}
shift
base=out/bench_${name}_$(date +%m%d_%H%M%S)
pkill -x bb-probe; sleep 1; pkill -9 -x bb-probe
: > out/pad; echo "${BASE_MASK:-0}" > out/toggles
# The game stays in the background (no focus taken from the desktop; BB_BENCH_FOREGROUND=1: brought
# to the front as usual). Input comes from the pad file either way.
[[ ${BB_BENCH_FOREGROUND:-0} == 1 ]] || export SDL_MAC_BACKGROUND_APP=1
env BB_GAME_DIR="${BB_GAME_DIR:-$HOME/Downloads/PS4/Bloodborne/CUSA03173}" \
    BB_PAD_FILE="$PWD/out/pad" BB_TOGGLE_FILE="$PWD/out/toggles" BB_FRAME_STATS=1 \
    BB_FRAME_LOG="$PWD/$base.frames.csv" BB_FPS_LIMIT="${BB_FPS_LIMIT:-0}" \
    BB_PRESENT_DUMP_TRIGGER="$PWD/out/grab.trigger" BB_SCREEN_DUMP_TRIGGER="$PWD/out/screen.trigger" \
    BB_MENU_TRIGGER="$PWD/out/menu.trigger" BB_DUMP_DIR="$PWD/out/grab" "$@" \
    bash run.sh > "$base.log" 2>&1 < /dev/null &
launcher=$!
fail() { echo "FAILED: $*  ($base.log)"; tail -20 "$base.log"; kill "$launcher" 2>/dev/null; pkill -x bb-probe; exit 1; }
# run.sh may still be building (sources changed): it counts as alive until it exits.
alive() { kill -0 "$launcher" 2>/dev/null || pgrep -x bb-probe > /dev/null; }
# Title: the pad is open and a few frame stats windows went by.
for i in $(seq 1 ${BOOT_WAIT:-240}); do
    sleep 1
    [[ $(grep -c '^Frame stats' "$base.log") -ge ${BOOT_WINDOWS:-3} ]] && grep -q 'pad opened' "$base.log" && break
    (( i > 20 )) && ! alive && fail "the game exited before the title"
done
sleep "${TITLE_WAIT:-6}"
tools/press.sh cross; sleep "${PRESS_GAP:-6}"; tools/press.sh cross
# Level: a scene with many draws.
start=$(date +%s)
for i in $(seq 1 $(( ${LEVEL_WAIT:-240} / 2 ))); do
    sleep 2
    alive || fail "the game exited while loading"
    last=$(grep '^Frame stats' "$base.log" | tail -1 | sed -E 's/.* ([0-9]+) draws\/frame.*/\1/')
    [[ -n $last && $last =~ ^[0-9]+$ && $last -gt 600 ]] && break
done
echo "level after $(( $(date +%s) - start )) s"
mark=$(grep -c '^Frame stats' "$base.log")
# TRACE=1: a Metal System Trace of ${TRACE_SECONDS:-5} s after the level settled (Xcode's xctrace;
# tools/mst_summary.py reads it).
if [[ ${TRACE:-0} == 1 ]]; then
    sleep 8
    DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer} xcrun xctrace record \
        --template 'Metal System Trace' --attach "$(pgrep -x bb-probe | head -1)" \
        --time-limit "${TRACE_SECONDS:-5}s" --output "$base.trace" > /dev/null 2>&1 &&
        echo "trace: $base.trace" || echo "trace failed"
fi
# SAMPLE=1: call stacks of every thread (macOS sample), $base.sample.txt: SAMPLE_SECONDS (5) long,
# SAMPLE_DELAY (5) s after the level loaded.
if [[ ${SAMPLE:-0} == 1 ]]; then
    sleep "${SAMPLE_DELAY:-5}"
    sample "$(pgrep -x bb-probe | head -1)" "${SAMPLE_SECONDS:-5}" -file "$PWD/$base.sample.txt" > /dev/null 2>&1 &&
        echo "sample: $base.sample.txt" || echo "sample failed"
fi
# GPU utilization (IOAccelerator, no sudo needed) and the game's CPU use, once a second.
: > "$base.load"
for ((t = 0; t < ${HOLD:-40}; ++t)); do
    gpu=$(ioreg -r -d 1 -c IOAccelerator | grep -oE '"Device Utilization %"=[0-9]+' | head -1 | cut -d= -f2)
    cpu=$(ps -o pcpu= -p "$(pgrep -x bb-probe | head -1)" 2>/dev/null | tr -d ' ')
    echo "$t ${gpu:-?} ${cpu:-?}" >> "$base.load"
    sleep 1
done
alive || fail "the game exited in the level"
awk 'NR > 3 {g += $2; c += $3; n++} END {if (n) printf "GPU utilization %.0f%%, game CPU %.0f%% (of one core)\n", g/n, c/n}' "$base.load"
pkill -x bb-probe; sleep 2; pkill -9 -x bb-probe
# Summary of the windows after the level settled.
tail -n +1 "$base.log" | grep '^Frame stats' | tail -n +$((mark + 2)) |
    sed -E 's/Frame stats: ([0-9.]+) FPS, worst frame ([0-9.]+) ms.*GPU thread ([0-9.]+) us\/draw, ([0-9]+) draws\/frame, idle ([0-9.]+)%.*/\1 \2 \4 \5/' |
    awk '{n++; f+=$1; w+=$2; d+=$3; id+=$4} END {if (n) printf "%s: %d windows, %.1f FPS, worst %.0f ms, %d draws/frame, GPU thread idle %.0f%%\n", "'"$name"'", n, f/n, w/n, d/n, id/n}'
# Frame times of the hold (the CSV's last ${HOLD} seconds): median, p90, share of time in frames
# over 150 ms (stalls).
python3 - "$base.frames.csv" "${HOLD:-40}" <<'PY'
import csv, sys
rows = [(float(r['t_s']), float(r['frame_ms'])) for r in csv.DictReader(open(sys.argv[1])) if r.get('frame_ms')]
end = rows[-1][0] if rows else 0
f = sorted(ms for t, ms in rows if t >= end - float(sys.argv[2]) + 2)
if f:
    total = sum(f)
    print(f"frames: {len(f)}, median {f[len(f) // 2]:.1f} ms, p90 {f[int(len(f) * 0.9)]:.1f} ms, "
          f"mean {1000 * len(f) / total:.1f} FPS, {100 * sum(x for x in f if x > 150) / total:.0f}% of time in frames over 150 ms")
PY
echo "$base.log"
