#!/usr/bin/env bash
# Exercise the actual interpreter, X11 window, button callbacks, timer and dry-run backend.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${KEYLOOM_BIN:-$ROOT/build/release/keyloom}
EVENT=${KEYLOOM_PICKER_EVENT:-$ROOT/build/release/picker_event}
LIFETIME=${KEYLOOM_GUI_LIFETIME:-$ROOT/build/release/gui_lifetime}
MAPPING_PROBE=${KEYLOOM_GUI_MAPPING_RUNTIME_PROBE:-$ROOT/build/release/gui_mapping_runtime_probe}
XCB_PROBE=${KEYLOOM_XCB_PROBE:-$ROOT/build/release/xcb_disconnect_probe}
STARTUP_PROBE=${KEYLOOM_GUI_STARTUP_PROBE:-$ROOT/build/release/gui_startup_probe}
SCRIPT=${KEYLOOM_GUI_SCRIPT:-$ROOT/examples/antiafk.loom}
command -v xdotool >/dev/null || exit 77
[[ -x $BIN && -x $EVENT && -x $LIFETIME && -x $XCB_PROBE && -x $STARTUP_PROBE ]] || exit 77
DIR=$(mktemp -d "${TMPDIR:-/tmp}/keyloom-gui.XXXXXXXX")
server= app= probe= startup_probe= map_probe=
cleanup() {
    local status=$?
    if [[ $status -ne 0 && $status -ne 77 ]]; then
        cat "$DIR/out" "$DIR/err" >&2 2>/dev/null || true
    fi
    [[ -z $app ]] || kill "$app" 2>/dev/null || true
    [[ -z $probe ]] || kill "$probe" 2>/dev/null || true
    [[ -z $map_probe ]] || kill "$map_probe" 2>/dev/null || true
    [[ -z $startup_probe ]] || kill "$startup_probe" 2>/dev/null || true
    [[ -z $server ]] || kill -CONT "$server" 2>/dev/null || true
    [[ -z $server ]] || kill "$server" 2>/dev/null || true
    [[ -z $app ]] || wait "$app" 2>/dev/null || true
    [[ -z $probe ]] || wait "$probe" 2>/dev/null || true
    [[ -z $map_probe ]] || wait "$map_probe" 2>/dev/null || true
    [[ -z $startup_probe ]] || wait "$startup_probe" 2>/dev/null || true
    [[ -z $server ]] || wait "$server" 2>/dev/null || true
    rm -f "$DIR/out" "$DIR/err" "$DIR/rects" "$DIR/external.loom" "$DIR/disconnect.loom" \
          "$DIR/xcb-control" "$DIR/xcb-out" "$DIR/xcb-err" \
          "$DIR/startup-control" "$DIR/startup-out" "$DIR/startup-err"
    rmdir "$DIR" 2>/dev/null || true
}
trap cleanup EXIT
D=
for number in $(seq 96 199); do
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
DISPLAY=$D "$LIFETIME"

# Isolated keyboard-layout probe (separate Xvfb): exercises GUI startup/input
# handling under alternate initial keyboard layouts. GuiHost now refreshes on
# XCB_MAPPING_NOTIFY; this provides deterministic Xvfb layout evidence.
if command -v setxkbmap >/dev/null && command -v Xvfb >/dev/null; then
    layout_d= layout_server=
    for number in $(seq 200 229); do
        [[ ! -e /tmp/.X11-unix/X$number ]] || continue
        layout_d=:$number
        Xvfb "$layout_d" -screen 0 1024x768x24 >/dev/null 2>&1 & layout_server=$!
        for _ in $(seq 1 50); do
            DISPLAY=$layout_d xdotool getmouselocation >/dev/null 2>&1 && break
            sleep 0.1
        done
        DISPLAY=$layout_d xdotool getmouselocation >/dev/null 2>&1 && break
        kill "$layout_server" 2>/dev/null || true; wait "$layout_server" 2>/dev/null || true
        layout_server= layout_d=
    done
    if [[ -n $layout_d ]]; then
        DISPLAY=$layout_d setxkbmap us >/dev/null 2>&1 || true
        DISPLAY=$layout_d "$LIFETIME"
        DISPLAY=$layout_d setxkbmap fr >/dev/null 2>&1 || DISPLAY=$layout_d setxkbmap de >/dev/null 2>&1 || true
        DISPLAY=$layout_d "$LIFETIME"
        # Runtime MappingNotify probe: change layout while GUI event loop runs.
        if [[ -x $MAPPING_PROBE ]]; then
            DISPLAY=$layout_d setxkbmap us >/dev/null 2>&1 || true
            DISPLAY=$layout_d "$MAPPING_PROBE" & map_probe=$!
            sleep 0.4
            DISPLAY=$layout_d setxkbmap fr >/dev/null 2>&1 || DISPLAY=$layout_d setxkbmap de >/dev/null 2>&1 || true
            sleep 0.4
            DISPLAY=$layout_d setxkbmap us >/dev/null 2>&1 || true
            wait "$map_probe"
            echo 'ok   GUI handles runtime MappingNotify/layout changes under Xvfb'
        fi
        kill "$layout_server" 2>/dev/null || true; wait "$layout_server" 2>/dev/null || true
        echo 'ok   GUI lifetime probe passes under alternate Xvfb keyboard layouts (us/fr/de)'
    fi
fi

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
# The auto-presser interval is at least 60s, so a short window is enough to
# show that no press is left pending after the stop.
sleep 0.6
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

cat >"$DIR/external.loom" <<'LOOM'
loom "external destroy" {
    gui "Destroy Test" { label "test" }
    when gui window closes { log "destroy-close-once" }
}
LOOM
DISPLAY=$D stdbuf -oL "$BIN" --run "$DIR/external.loom" --dry-run >"$DIR/out" 2>"$DIR/err" & app=$!
window=
for _ in $(seq 1 100); do
    kill -0 "$app" 2>/dev/null || { cat "$DIR/err" >&2; exit 1; }
    window=$(DISPLAY=$D xdotool search --name '^Destroy Test$' 2>/dev/null | head -1 || true)
    [[ -n $window ]] && grep -q 'Running ' "$DIR/out" && break
    sleep 0.1
done
[[ -n $window ]] || { echo 'destroy test GUI did not appear' >&2; exit 1; }
DISPLAY=$D "$EVENT" "$window" destroy
for _ in $(seq 1 50); do
    grep -q 'destroy-close-once' "$DIR/out" && break
    kill -0 "$app" 2>/dev/null || { cat "$DIR/err" >&2; exit 1; }
    sleep 0.1
done
[[ $(grep -c 'destroy-close-once' "$DIR/out") == 1 ]]
kill -0 "$app"
kill -TERM "$app"
wait "$app"
app=
[[ ! -s $DIR/err ]] || { cat "$DIR/err" >&2; exit 1; }
echo 'ok   external window destruction dispatches one close event'

cat >"$DIR/disconnect.loom" <<'LOOM'
loom "display disconnect" {
    gui "Disconnect Test" { input status = "test" }
    when timer every 5ms { call gui_set_text("status", "updating") }
}
LOOM
DISPLAY=$D stdbuf -oL "$BIN" --run "$DIR/disconnect.loom" --dry-run >"$DIR/out" 2>"$DIR/err" & app=$!
for _ in $(seq 1 100); do
    kill -0 "$app" 2>/dev/null || { cat "$DIR/err" >&2; exit 1; }
    grep -q 'Running ' "$DIR/out" && break
    sleep 0.1
done
grep -q 'Running ' "$DIR/out"
if [[ -x $XCB_PROBE ]]; then
    mkfifo "$DIR/xcb-control"
    exec 3<>"$DIR/xcb-control"
    DISPLAY=$D "$XCB_PROBE" <"$DIR/xcb-control" >"$DIR/xcb-out" 2>"$DIR/xcb-err" & probe=$!
    for _ in $(seq 1 50); do
        grep -q '^ready$' "$DIR/xcb-out" && break
        kill -0 "$probe" 2>/dev/null || { cat "$DIR/xcb-err" >&2; exit 1; }
        sleep 0.1
    done
    grep -q '^ready$' "$DIR/xcb-out"
    mkfifo "$DIR/startup-control"
    exec 4<>"$DIR/startup-control"
    DISPLAY=$D "$STARTUP_PROBE" <"$DIR/startup-control" >"$DIR/startup-out" 2>"$DIR/startup-err" & startup_probe=$!
    for _ in $(seq 1 50); do
        grep -q '^ready$' "$DIR/startup-out" && break
        kill -0 "$startup_probe" 2>/dev/null || { cat "$DIR/startup-err" >&2; exit 1; }
        sleep 0.1
    done
    grep -q '^ready$' "$DIR/startup-out"
    kill -STOP "$server"
    printf x >&3
    printf x >&4
    for _ in $(seq 1 50); do
        grep -q '^requesting$' "$DIR/xcb-out" && break
        kill -0 "$probe" 2>/dev/null || { cat "$DIR/xcb-err" >&2; exit 1; }
        sleep 0.1
    done
    grep -q '^requesting$' "$DIR/xcb-out"
    for _ in $(seq 1 50); do
        grep -q '^creating$' "$DIR/startup-out" && break
        kill -0 "$startup_probe" 2>/dev/null || { cat "$DIR/startup-err" >&2; exit 1; }
        sleep 0.1
    done
    grep -q '^creating$' "$DIR/startup-out"
    sleep 0.1
    kill -KILL "$server"
fi
wait "$server" 2>/dev/null || true
# SIGKILL cannot let Xvfb unlink its socket. Remove only the lock/socket
# belonging to the server this test started, never another running display.
lock="/tmp/.X${D#:}-lock"
if [[ -f $lock && $(tr -d '[:space:]' < "$lock") == "$server" ]]; then
    rm -f "$lock" "/tmp/.X11-unix/X${D#:}"
fi
server=
if [[ -n $probe ]]; then
    wait "$probe" || { cat "$DIR/xcb-err" >&2; exit 1; }
    probe=
    exec 3>&-
    grep '^ok ' "$DIR/xcb-out"
fi
if [[ -n $startup_probe ]]; then
    wait "$startup_probe" || { cat "$DIR/startup-err" >&2; exit 1; }
    startup_probe=
    exec 4>&-
    grep '^ok ' "$DIR/startup-out"
fi
for _ in $(seq 1 50); do
    kill -0 "$app" 2>/dev/null || break
    sleep 0.1
done
if kill -0 "$app" 2>/dev/null; then echo 'display disconnect did not stop Keyloom' >&2; exit 1; fi
wait "$app"
app=
grep -q 'Stopped;' "$DIR/out"
[[ ! -s $DIR/err ]] || { cat "$DIR/err" >&2; exit 1; }
echo 'ok   X-server disconnect closes the panel and stops cleanly'
