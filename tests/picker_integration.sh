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
LIFECYCLE=${KEYLOOM_PICKER_LIFECYCLE:-$ROOT/build/release/picker_lifecycle}
UPLOAD_PROBE=${KEYLOOM_PICKER_UPLOAD_PROBE:-$ROOT/build/release/picker_upload_probe}
D=${PICKER_TEST_DISPLAY:-}
# The display numbers this run may use, as a comma separated list. The two
# picker variants get their own range so they can run at the same time.
DISPLAY_NUMBERS=${PICKER_TEST_DISPLAY_NUMBERS:-91,92,93,94,95}
DISPLAY_NUMBERS=${DISPLAY_NUMBERS//,/ }
SERVER_PID=
SERVER_KIND=
PIXEL_DIR=
UPLOAD_DIR=
UPLOAD_PID=
SCREEN=1368x768
SCREEN_DEPTH=${PICKER_TEST_DEPTH:-24}
TITLE="Keyloom mouse position"

skip() { echo "SKIP: $*"; exit 77; }
fail() { echo "FAIL: $*"; exit 1; }

[[ -x "$BIN" ]] || fail "build keyloom first (expected $BIN)"
[[ -x "$EVENT" ]] || skip "build the picker_event helper first (expected $EVENT)"
[[ -x "$LIFECYCLE" ]] || skip "build the picker_lifecycle helper first (expected $LIFECYCLE)"
[[ -x "$UPLOAD_PROBE" ]] || skip "build the picker_upload_probe helper first (expected $UPLOAD_PROBE)"
command -v xdotool >/dev/null 2>&1 || skip "xdotool is not installed"

start_server() {
    local display=$1
    if [[ ${PICKER_TEST_SERVER:-} == xwayland ]] && command -v Xwayland >/dev/null 2>&1 && [[ -n ${WAYLAND_DISPLAY:-} ]]; then
        Xwayland "$display" -rootless >/dev/null 2>&1 &
        SERVER_PID=$!
        SERVER_KIND=xwayland
    elif command -v Xvfb >/dev/null 2>&1; then
        Xvfb "$display" -screen 0 "${SCREEN}x${SCREEN_DEPTH}" >/dev/null 2>&1 &
        SERVER_PID=$!
        SERVER_KIND=xvfb
    elif command -v Xwayland >/dev/null 2>&1 && [[ -n ${WAYLAND_DISPLAY:-} ]]; then
        Xwayland "$display" -rootless >/dev/null 2>&1 &
        SERVER_PID=$!
        SERVER_KIND=xwayland
    else
        return 1
    fi
    for _ in $(seq 1 200); do
        if DISPLAY=$display xdotool getmouselocation >/dev/null 2>&1; then return 0; fi
        sleep 0.02
    done
    return 1
}

cleanup() {
    if [[ -n ${UPLOAD_PID:-} ]]; then
        kill "$UPLOAD_PID" 2>/dev/null
        wait "$UPLOAD_PID" 2>/dev/null
    fi
    if [[ -n ${UPLOAD_DIR:-} ]]; then
        rm -f "$UPLOAD_DIR/control" "$UPLOAD_DIR/out" "$UPLOAD_DIR/err"
        rmdir "$UPLOAD_DIR" 2>/dev/null
    fi
    if [[ -n ${PIXEL_DIR:-} ]]; then
        rm -f "$PIXEL_DIR/scrot"
        rmdir "$PIXEL_DIR" 2>/dev/null
    fi
    if [[ -n ${SERVER_PID:-} ]]; then
        kill -CONT "$SERVER_PID" 2>/dev/null
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
    fi
}
trap cleanup EXIT

if [[ -z $D ]]; then
    for number in $DISPLAY_NUMBERS; do
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
    DISPLAY=$D PATH=${PICKER_PATH:-$PATH} "$BIN" --get-mouse-pos >"$out" 2>"$err" &
    pid=$!
    OVERLAY=
    for _ in $(seq 1 500); do
        kill -0 "$pid" 2>/dev/null || break
        OVERLAY=$(DISPLAY=$D xdotool search --onlyvisible --name "^$TITLE$" 2>/dev/null | head -1)
        [[ -n $OVERLAY ]] && break
        sleep 0.02
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
    # The picker selects its input masks and takes the input focus once the
    # overlay is viewable. Waiting for the focus to land on the overlay is the
    # real readiness signal; a fixed sleep only approximates it.
    for _ in $(seq 1 500); do
        [[ $(DISPLAY=$D xdotool getwindowfocus 2>/dev/null) == "$OVERLAY" ]] && break
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.02
    done
    eval "$action"
    for _ in $(seq 1 200); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.02
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

if [[ $SERVER_KIND == xvfb ]] && command -v convert >/dev/null 2>&1; then
    PIXEL_DIR=$(mktemp -d)
    convert_bin=$(command -v convert)
    printf '#!/bin/sh\nexec "%s" -size 1368x768 "xc:#c03471" "$1"\n' "$convert_bin" >"$PIXEL_DIR/scrot"
    chmod +x "$PIXEL_DIR/scrot"
    PICKER_PATH=$PIXEL_DIR
    run_picker 'PIXEL=$(DISPLAY=$D "$EVENT" "$OVERLAY" pixel 300 300); DISPLAY=$D "$EVENT" "$OVERLAY" button 1 300 300'
    unset PICKER_PATH
    expected_pixel=c03471
    [[ $SCREEN_DEPTH != 16 ]] || expected_pixel=00b9ae
    [[ $RESULT_RC -eq 0 && $PIXEL == "$expected_pixel" ]] || fail "picker screenshot pixel was $PIXEL, expected $expected_pixel: $RESULT_ERR"
    rm -f "$PIXEL_DIR/scrot"
    rmdir "$PIXEL_DIR"
    PIXEL_DIR=
    echo 'ok   picker renders screenshot pixels'
fi

# Exercise the picker as a library, where the process and its previous signal
# and X error handlers must survive after a cancelled pick.
original_bin=$BIN
BIN=$LIFECYCLE
run_picker 'DISPLAY=$D "$EVENT" "$OVERLAY" button 3 400 300'
BIN=$original_bin
[[ $RESULT_RC -eq 0 ]] || fail "picker lifecycle check failed: $RESULT_ERR"
[[ $RESULT_OUT == 'ok   picker restores signal and X error handlers' ]] || fail "unexpected lifecycle output: $RESULT_OUT"
echo "$RESULT_OUT"

DISPLAY=$D "$UPLOAD_PROBE" cancel || fail 'picker upload cancellation cleanup failed'

# Only terminate a server created by this script, never PICKER_TEST_DISPLAY.
if [[ -n $SERVER_PID ]]; then
    # Freeze the server while the picker keeps querying pointer/window state,
    # then kill it with an XCB reply outstanding.
    original_bin=$BIN
    BIN=$LIFECYCLE
    export PICKER_EXPECT_DISCONNECT=1
    run_picker 'kill -STOP "$SERVER_PID"; sleep 0.3; kill -KILL "$SERVER_PID"; wait "$SERVER_PID" 2>/dev/null; lock="/tmp/.X${D#:}-lock"; if [[ -f $lock && $(tr -d "[:space:]" < "$lock") == "$SERVER_PID" ]]; then rm -f "$lock" "/tmp/.X11-unix/X${D#:}"; fi; SERVER_PID='
    unset PICKER_EXPECT_DISCONNECT
    BIN=$original_bin
    [[ $RESULT_RC -eq 0 ]] || fail "disconnected picker lifecycle failed ($RESULT_RC): $RESULT_ERR"
    [[ $RESULT_OUT == 'ok   picker disconnect releases fd and restores handlers' ]] || fail "unexpected disconnect output: $RESULT_OUT"
    echo 'ok   picker survives server loss during an X11 operation'

    # The upload hook pauses after xcb_put_image_checked and before its check.
    # Kill a fresh private server at that exact boundary, then verify cleanup.
    start_server "$D" || fail 'could not restart private X server for upload test'
    UPLOAD_DIR=$(mktemp -d)
    mkfifo "$UPLOAD_DIR/control"
    exec 3<>"$UPLOAD_DIR/control"
    DISPLAY=$D "$UPLOAD_PROBE" disconnect <"$UPLOAD_DIR/control" >"$UPLOAD_DIR/out" 2>"$UPLOAD_DIR/err" &
    UPLOAD_PID=$!
    for _ in $(seq 1 500); do
        grep -q '^uploading$' "$UPLOAD_DIR/out" && break
        kill -0 "$UPLOAD_PID" 2>/dev/null || { cat "$UPLOAD_DIR/err" >&2; fail 'upload probe exited early'; }
        sleep 0.02
    done
    grep -q '^uploading$' "$UPLOAD_DIR/out" || fail 'upload probe did not reach checked request'
    kill -KILL "$SERVER_PID"
    wait "$SERVER_PID" 2>/dev/null
    lock="/tmp/.X${D#:}-lock"
    if [[ -f $lock && $(tr -d '[:space:]' < "$lock") == "$SERVER_PID" ]]; then
        rm -f "$lock" "/tmp/.X11-unix/X${D#:}"
    fi
    SERVER_PID=
    printf x >&3
    wait "$UPLOAD_PID" || { cat "$UPLOAD_DIR/err" >&2; fail 'upload disconnect cleanup failed'; }
    UPLOAD_PID=
    exec 3>&-
    grep '^ok ' "$UPLOAD_DIR/out" || fail 'upload probe did not report success'
    rm -f "$UPLOAD_DIR/control" "$UPLOAD_DIR/out" "$UPLOAD_DIR/err"
    rmdir "$UPLOAD_DIR"
    UPLOAD_DIR=
fi

echo "picker integration tests passed"
