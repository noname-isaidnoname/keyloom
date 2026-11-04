#!/usr/bin/env bash
# Exercise the actual interpreter, X11 window, button callbacks, timer and dry-run backend.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${KEYLOOM_BIN:-$ROOT/build/release/keyloom}
EVENT=${KEYLOOM_PICKER_EVENT:-$ROOT/build/release/picker_event}
SCRIPT=${KEYLOOM_GUI_SCRIPT:-$ROOT/examples/antiafk.loom}
command -v xdotool >/dev/null || exit 77
[[ -x $BIN && -x $EVENT ]] || exit 77
DIR=$(mktemp -d "${TMPDIR:-/tmp}/keyloom-gui.XXXXXXXX")
server= app=
cleanup() {
    local status=$?
    if [[ $status -ne 0 && $status -ne 77 ]]; then
        cat "$DIR/out" "$DIR/err" >&2 2>/dev/null || true
    fi
    [[ -z $app ]] || kill "$app" 2>/dev/null || true
    [[ -z $server ]] || kill "$server" 2>/dev/null || true
    [[ -z $app ]] || wait "$app" 2>/dev/null || true
    [[ -z $server ]] || wait "$server" 2>/dev/null || true
    rm -f "$DIR/out" "$DIR/err" "$DIR/rects"
    rmdir "$DIR" 2>/dev/null || true
}
trap cleanup EXIT
D=
for number in 96 97 98 99; do
    [[ ! -e /tmp/.X11-unix/X$number ]] || continue
    D=:$number
    if command -v Xvfb >/dev/null; then
        Xvfb "$D" -screen 0 1024x768x24 > /dev/null 2>&1 & server=$!
    elif command -v Xwayland >/dev/null && [[ -n ${WAYLAND_DISPLAY:-} ]]; then
        Xwayland "$D" -rootless > /dev/null 2>&1 & server=$!
    else
        exit 77
    fi
    for _ in $(seq 1 50); do
        DISPLAY=$D xdotool getmouselocation >/dev/null 2>&1 && break
        sleep 0.1
    done
    DISPLAY=$D xdotool getmouselocation >/dev/null 2>&1 && break
    kill "$server" 2>/dev/null || true
    wait "$server" 2>/dev/null || true
    server= D=
done
[[ -n $D ]] || exit 77
DISPLAY=$D KEYLOOM_GUI_DEBUG_RECTS="$DIR/rects" stdbuf -oL "$BIN" --run "$SCRIPT" --dry-run >"$DIR/out" 2>"$DIR/err" & app=$!
window=
for _ in $(seq 1 100); do
    kill -0 "$app" 2>/dev/null || { cat "$DIR/err" >&2; exit 1; }
    window=$(DISPLAY=$D xdotool search --name '^Space Auto-Presser$' 2>/dev/null | head -1 || true)
    [[ -n $window && -s $DIR/rects ]] && break
    sleep 0.1
done
[[ -n $window && -s $DIR/rects ]] || { echo 'GUI did not appear' >&2; exit 1; }
click_widget() {
    local id=$1 x y w h kind name
    read -r kind name x y w h < <(awk -v id="$id" '$2 == id { print; exit }' "$DIR/rects")
    DISPLAY=$D "$EVENT" "$window" button 1 "$((x+w/2))" "$((y+h/2))"
}
set_input() {
    click_widget "$1"
    DISPLAY=$D "$EVENT" "$window" key Delete
    for key in 1 period 0; do DISPLAY=$D "$EVENT" "$window" key "$key"; done
}
set_input min_interval
set_input max_interval
click_widget toggle
for _ in $(seq 1 50); do
    grep -q 'Auto-presser started' "$DIR/out" && grep -q '\[dry-run\] key SPACE press' "$DIR/out" && break
    kill -0 "$app" 2>/dev/null || { cat "$DIR/err" >&2; exit 1; }
    sleep 0.1
done
grep -q 'Auto-presser started' "$DIR/out"
grep -q '\[dry-run\] key SPACE press' "$DIR/out"
kill -0 "$app"
click_widget toggle
for _ in $(seq 1 50); do
    grep -q 'Auto-presser stopped' "$DIR/out" && grep -q '\[dry-run\] key SPACE free' "$DIR/out" && break
    sleep 0.1
done
grep -q 'Auto-presser stopped' "$DIR/out"
grep -q '\[dry-run\] key SPACE free' "$DIR/out"
kill -0 "$app"
count=$(grep -c '\[dry-run\] key SPACE press' "$DIR/out")
sleep 1.2
[[ $(grep -c '\[dry-run\] key SPACE press' "$DIR/out") == "$count" ]]
click_widget toggle
for _ in $(seq 1 50); do
    [[ $(grep -c 'Auto-presser started' "$DIR/out") -ge 2 ]] && break
    sleep 0.1
done
[[ $(grep -c 'Auto-presser started' "$DIR/out") -ge 2 ]]
kill -0 "$app"
DISPLAY=$D "$EVENT" "$window" close
for _ in $(seq 1 50); do
    kill -0 "$app" 2>/dev/null || break
    sleep 0.1
done
if kill -0 "$app" 2>/dev/null; then echo 'window close did not stop Keyloom' >&2; exit 1; fi
wait "$app"
app=
echo 'ok   persistent anti-AFK panel: edit fields, start, stop and remain open'
