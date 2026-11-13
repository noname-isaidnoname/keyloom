#!/usr/bin/env bash
# Exercises real /dev/uinput setup failure handling via LD_PRELOAD ioctl fault
# injection (no kernel fault injection/sudo). Complements deterministic
# UInputSetupStep seams by running UInputDevice setup against a real fd.
set -euo pipefail
# The sanitizer runtime insists on being the first library in the process. The
# fault injection below preloads the shim, so the check is disabled; the
# sanitizer itself stays on.
if [[ -n ${ASAN_OPTIONS:-} ]]; then
    export ASAN_OPTIONS="$ASAN_OPTIONS:verify_asan_link_order=0"
else
    export ASAN_OPTIONS="verify_asan_link_order=0"
fi
PROBE=${LOOMWRIGHT_UINPUT_FAULT_PROBE:-}
SHIM=${LOOMWRIGHT_UINPUT_IOCTL_FAULT_SHIM:-}
[[ -x $PROBE && -f $SHIM ]] || exit 77
[[ -w /dev/uinput ]] || exit 77

fail_at() {
    local step=$1
    local out
    out=$(LD_PRELOAD=$SHIM LOOMWRIGHT_FAIL_UIOCTL_STEP="$step" "$PROBE" "$step" 2>&1) || {
        echo "$out" >&2
        echo "uinput fault probe failed for step: $step" >&2
        return 1
    }
    printf '%s\n' "$out" | grep -q "^ok   real /dev/uinput ioctl fault at $step:" || {
        echo "$out" >&2
        echo "unexpected output for step: $step" >&2
        return 1
    }
    printf '%s\n' "$out" | grep '^ok   ' || true
}

fail_at_with() {
    local step=$1 config=$2
    local out
    out=$(LD_PRELOAD=$SHIM LOOMWRIGHT_FAIL_UIOCTL_STEP="$step" "$PROBE" "$step" "$config" 2>&1) || {
        echo "$out" >&2
        echo "uinput fault probe failed for step: $step ($config)" >&2
        return 1
    }
    printf '%s\n' "$out" | grep -q "^ok   real /dev/uinput ioctl fault at $step:" || {
        echo "$out" >&2
        echo "unexpected output for step: $step ($config)" >&2
        return 1
    }
    printf '%s\n' "$out" | grep '^ok   ' || true
}

# Default config (key only): critical real-fd setup steps.
fail_at_with "UI_SET_EVBIT EV_KEY" default
fail_at_with "UI_SET_KEYBIT" default
fail_at_with "UI_DEV_SETUP" default
fail_at_with "UI_DEV_CREATE" default
# REL/ABS/PHYS configs exercise additional real-fd ioctl paths (no sudo).
fail_at_with "UI_SET_EVBIT EV_REL" rel
fail_at_with "UI_SET_RELBIT" rel
fail_at_with "UI_SET_EVBIT EV_ABS" abs
fail_at_with "UI_SET_ABSBIT" abs
fail_at_with "UI_ABS_SETUP" abs
fail_at_with "UI_SET_PHYS" phys
echo "ok   real /dev/uinput ioctl fault injection (LD_PRELOAD) covers all tested setup steps"