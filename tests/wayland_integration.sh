#!/usr/bin/env bash
set -euo pipefail
command -v weston >/dev/null || { echo 'SKIP: weston is not installed'; exit 77; }
dir=$(mktemp -d "${TMPDIR:-/tmp}/loomwright-wayland.XXXXXXXX")
server= app= xserver= stage='starting Weston'
cleanup() {
    local result=$?
    if [[ $result -ne 0 && $result -ne 77 ]]; then
        echo "Wayland integration failed during: $stage" >&2
        cat "$dir/out" "$dir/probe.log" "$dir/weston.log" >&2 2>/dev/null || true
    fi
    [[ -z $app ]] || kill "$app" 2>/dev/null || true
    [[ -z $server ]] || kill "$server" 2>/dev/null || true
    [[ -z $xserver ]] || kill "$xserver" 2>/dev/null || true
    [[ -z $app ]] || wait "$app" 2>/dev/null || true
    [[ -z $server ]] || wait "$server" 2>/dev/null || true
    [[ -z $xserver ]] || wait "$xserver" 2>/dev/null || true
    rm -f "$dir/weston.log" "$dir/out" "$dir/probe.log" "$dir/script.loom" "$dir/loomwright-test" "$dir/loomwright-test.lock"
    rmdir "$dir" 2>/dev/null || true
}
trap cleanup EXIT
chmod 700 "$dir"
export XDG_RUNTIME_DIR=$dir XDG_SESSION_TYPE=wayland WAYLAND_DISPLAY=loomwright-test
unset DISPLAY
weston --backend=headless-backend.so --socket="$WAYLAND_DISPLAY" --idle-time=0 --log="$dir/weston.log" >/dev/null 2>&1 &
server=$!
for _ in $(seq 1 400); do
    [[ -S $dir/$WAYLAND_DISPLAY ]] && break
    kill -0 "$server" 2>/dev/null || { cat "$dir/weston.log" >&2; exit 1; }
    sleep 0.05
done
[[ -S $dir/$WAYLAND_DISPLAY ]] || { cat "$dir/weston.log" >&2; exit 1; }
stage='native panel probe'
"$LOOMWRIGHT_WAYLAND_PROBE" >"$dir/probe.log" 2>&1
cat "$dir/probe.log"
cat > "$dir/script.loom" <<'EOF'
loom "native-test" {
    option compositor = "wayland"
    gui "Native script" { label "Ready" }
    when app starts { log "native-script-started" }
}
EOF
run_script() {
    "$LOOMWRIGHT_BIN" --run "$dir/script.loom" --dry-run >"$dir/out" 2>&1 &
    app=$!
    for _ in $(seq 1 400); do
        grep -q native-script-started "$dir/out" && break
        kill -0 "$app" 2>/dev/null || { cat "$dir/out" >&2; exit 1; }
        sleep 0.05
    done
    grep -q native-script-started "$dir/out" || { echo 'script did not start within 20 seconds' >&2; cat "$dir/out" >&2; exit 1; }
    # Non-interactive shells can start background jobs with SIGINT ignored.
    kill -TERM "$app"
    local status=0
    wait "$app" || status=$?
    app=
    [[ $status == 0 || $status == 130 ]]
}
stage='explicit Wayland startup'
run_script
stage='explicit Wayland with WAYLAND_SOCKET'
export WAYLAND_SOCKET=999999
run_script
unset WAYLAND_SOCKET
stage='strict compositor selection'
if XDG_SESSION_TYPE=x11 "$LOOMWRIGHT_BIN" --run "$dir/script.loom" --dry-run >"$dir/out" 2>&1; then
    echo 'explicit Wayland unexpectedly accepted an X11 session' >&2; exit 1
fi
grep -q 'cannot run in an X11 session' "$dir/out"
sed -i 's/compositor = "wayland"/compositor = "x11"/' "$dir/script.loom"
if "$LOOMWRIGHT_BIN" --run "$dir/script.loom" --dry-run >"$dir/out" 2>&1; then
    echo 'explicit X11 unexpectedly worked without an X display' >&2; exit 1
fi
grep -q 'requires an accessible X11' "$dir/out"
sed -i 's/compositor = "x11"/compositor = "auto"/' "$dir/script.loom"
stage='automatic Wayland selection'
run_script
sed -i '/option compositor/d' "$dir/script.loom"
stage='default compositor selection'
run_script
echo 'ok   explicit, auto and default compositor selection without X11'

if [[ ${LOOMWRIGHT_TEST_HOOKS:-} == 1 ]] && command -v Xvfb >/dev/null; then
    stage='X11 fallback setup'
    for number in $(seq 250 279); do
        [[ ! -e /tmp/.X11-unix/X$number && ! -e /tmp/.X$number-lock ]] || continue
        Xvfb ":$number" -screen 0 800x600x24 > /dev/null 2>&1 &
        xserver=$!
        export DISPLAY=:$number
        for _ in $(seq 1 200); do
            [[ -S /tmp/.X11-unix/X$number ]] && break
            kill -0 "$xserver" 2>/dev/null || break
            sleep 0.05
        done
        [[ -S /tmp/.X11-unix/X$number ]] && break
        wait "$xserver" 2>/dev/null || true
        xserver=
    done
    [[ -n $xserver ]] || { echo 'could not start Xvfb for fallback test' >&2; exit 1; }
    export LOOMWRIGHT_TEST_FAIL_WAYLAND_STARTUP=1
    stage='automatic X11 fallback'
    run_script
    sed -i '/loom "native-test" {/a\    option compositor = "wayland"' "$dir/script.loom"
    stage='strict Wayland startup failure'
    if "$LOOMWRIGHT_BIN" --run "$dir/script.loom" --dry-run >"$dir/out" 2>&1; then
        echo 'explicit Wayland unexpectedly fell back to X11' >&2; exit 1
    fi
    grep -q 'injected Wayland panel startup failure' "$dir/out"
    unset LOOMWRIGHT_TEST_FAIL_WAYLAND_STARTUP DISPLAY
    kill "$xserver"
    wait "$xserver" || true
    xserver=
    echo 'ok   automatic fallback and strict explicit Wayland selection'
fi

# A compositor disconnect must not let GTK terminate the automation process
# before it releases output holds and reports the GUI error.
stage='compositor disconnect cleanup'
cat > "$dir/script.loom" <<'EOF'
loom "native-disconnect" {
    option compositor = "wayland"
    gui "Native script" { label "Ready" }
    when app starts {
        mouse left hold as test_hold
        log "native-script-started"
    }
}
EOF
"$LOOMWRIGHT_BIN" --run "$dir/script.loom" --dry-run >"$dir/out" 2>&1 &
app=$!
for _ in $(seq 1 400); do
    grep -q native-script-started "$dir/out" && break
    kill -0 "$app" 2>/dev/null || { cat "$dir/out" >&2; exit 1; }
    sleep 0.05
done
grep -q native-script-started "$dir/out"
kill "$server"
wait "$server" || true
server=
for _ in $(seq 1 400); do
    kill -0 "$app" 2>/dev/null || break
    sleep 0.05
done
if kill -0 "$app" 2>/dev/null; then echo 'runtime hung after compositor loss' >&2; exit 1; fi
status=0
wait "$app" || status=$?
app=
[[ $status -ne 0 ]]
grep -q 'Native Wayland panel connection lost' "$dir/out"
grep -q 'Stopped; cleanup attempted' "$dir/out"
grep -Fq '[dry-run] mouse left free' "$dir/out"
echo 'ok   compositor loss stops the runtime through normal cleanup'
