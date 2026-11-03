#!/usr/bin/env bash
# Real end-to-end test for `keyloom --get-mouse-pos`.
#
# It starts a private X server (Xwayland or Xvfb), shows the actual picker in
# it, sends real X button/key events to the overlay, and checks the printed
# coordinates. This is the test that catches the Xlib BadMatch race on
# XSetInputFocus: the picker must come up and stay up without any pointer
# movement or click.
#
# Exit codes: 0 pass, 1 failure, 77 skipped (no X server, xdotool, event helper
# or screenshot tool available on this machine).
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${KEYLOOM_BIN:-$ROOT/build/release/keyloom}
EVENT=${KEYLOOM_PICKER_EVENT:-$ROOT/build/release/picker_event}
D=${PICKER_TEST_DISPLAY:-}
SERVER_PID=
SCREEN=1368x768
TITLE="Keyloom mouse position"

skip() { echo "SKIP: $*"; exit 77; }
fail() { echo "FAIL: $*"; exit 1; }

[[ -x "$BIN" ]] || fail "build keyloom first (expected $BIN)"
[[ -x "$EVENT" ]] || skip "build the picker_event helper first (expected $EVENT)"
command -v xdotool >/dev/null 2>&1 || skip "xdotool is not installed"

start_server() {
    local display=$1
    if command -v Xwayland >/dev/null 2>&1 && [[ -n ${WAYLAND_DISPLAY:-} ]]; then
        Xwayland "$display" -rootless >/dev/null 2>&1 &
        SERVER_PID=$!
    elif command -v Xvfb >/dev/null 2>&1; then
        Xvfb "$display" -screen 0 "${SCREEN}x24" >/dev/null 2>&1 &
        SERVER_PID=$!
    else
        return 1
    fi
    for _ in $(seq 1 50); do
        if DISPLAY=$display xdotool getmouselocation >/dev/null 2>&1; then return 0; fi
        sleep 0.1
    done
    return 1
}

cleanup() {
    if [[ -n ${SERVER_PID:-} ]]; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
    fi
}
trap cleanup EXIT

if [[ -z $D ]]; then
    for number in 91 92 93 94 95; do
        if [[ ! -e "/tmp/.X11-unix/X$number" ]]; then
            D=:$number
            start_server "$D" && break
            D=
        fi
    done
    [[ -n $D ]] || skip "could not start a private X server (install Xvfb or run inside Wayland with Xwayland)"
else
    DISPLAY=$D xdotool getmouselocation >/dev/null 2>&1 ||
        skip "PICKER_TEST_DISPLAY=$D is not reachable"
fi

# Start the picker, wait until the overlay is on screen without touching the
# pointer or a button (this is where XSetInputFocus used to race the window
# manager and abort the process), then run `action` with OVERLAY set to the
# picker window.
run_picker() {
    local action=$1 out err pid rc
    out=$(mktemp)
    err=$(mktemp)
    DISPLAY=$D "$BIN" --get-mouse-pos >"$out" 2>"$err" &
    pid=$!
    OVERLAY=
    for _ in $(seq 1 150); do
        kill -0 "$pid" 2>/dev/null || break
        OVERLAY=$(DISPLAY=$D xdotool search --name "^$TITLE$" 2>/dev/null | head -1)
        [[ -n $OVERLAY ]] && break
        sleep 0.1
    done
    if [[ -z $OVERLAY ]]; then
        if kill -0 "$pid" 2>/dev/null; then
            kill -9 "$pid" 2>/dev/null
            fail "picker never showed its overlay"
        fi
        wait "$pid"
        rc=$?
        cat "$err" >&2
        if grep -q "screenshot tool" "$err"; then
            rm -f "$out" "$err"
            skip "no usable screenshot tool for this display"
        fi
        fail "picker died before the overlay was usable (rc=$rc); this is the XSetInputFocus race"
    fi
    # Give the event loop a moment to select its input masks.
    sleep 1
    eval "$action"
    for _ in $(seq 1 100); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.05
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill -9 "$pid" 2>/dev/null
        fail "picker did not exit after $action"
    fi
    wait "$pid"
    rc=$?
    RESULT_OUT=$(cat "$out")
    RESULT_ERR=$(cat "$err")
    RESULT_RC=$rc
    rm -f "$out" "$err"
}

# 1. Left click. The overlay has been up for a second without interaction, so
#    this also exercises the focus race. The printed coordinate must match the
#    click.
run_picker 'DISPLAY=$D "$EVENT" "$OVERLAY" button 1 300 200'
[[ $RESULT_RC -eq 0 ]] || fail "left click exit code $RESULT_RC, expected 0; stderr: $RESULT_ERR"
[[ $RESULT_OUT == "300, 200" ]] || fail "left click printed '$RESULT_OUT', expected '300, 200'"
[[ $RESULT_ERR != *"X Error"* ]] || fail "the picker lets an X error escape: $RESULT_ERR"
echo "ok   picker left click -> $RESULT_OUT"

# 2. Right click cancels with 130 and prints nothing.
run_picker 'DISPLAY=$D "$EVENT" "$OVERLAY" button 3 400 300'
[[ $RESULT_RC -eq 130 ]] || fail "right click exit code $RESULT_RC, expected 130"
[[ -z $RESULT_OUT ]] || fail "right click printed '$RESULT_OUT', expected nothing"
[[ $RESULT_ERR == *cancelled* ]] || fail "right click did not report a clean cancel: $RESULT_ERR"
echo "ok   picker right click cancels"

# 3. Escape cancels.
run_picker 'DISPLAY=$D "$EVENT" "$OVERLAY" key Escape'
[[ $RESULT_RC -eq 130 ]] || fail "escape exit code $RESULT_RC, expected 130"
[[ $RESULT_ERR == *cancelled* ]] || fail "escape did not report a clean cancel: $RESULT_ERR"
echo "ok   picker escape cancels"

# 4. SIGINT cancels instead of hanging on a stuck event loop.
run_picker 'kill -INT $pid'
[[ $RESULT_RC -eq 130 ]] || fail "SIGINT exit code $RESULT_RC, expected 130"
[[ $RESULT_ERR == *cancelled* ]] || fail "SIGINT did not report a clean cancel: $RESULT_ERR"
echo "ok   picker SIGINT cancels"

echo "picker integration tests passed"
