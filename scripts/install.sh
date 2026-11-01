#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

# Install into an existing environment. This script never creates a venv:
# activate the environment you want first (that is how uv users usually run),
# or point PYTHON at its interpreter.

python_bin=""
if [[ -n "${PYTHON:-}" ]]; then
    python_bin="$PYTHON"
elif [[ -n "${VIRTUAL_ENV:-}" && -x "${VIRTUAL_ENV}/bin/python" ]]; then
    python_bin="${VIRTUAL_ENV}/bin/python"
elif [[ -n "${CONDA_PREFIX:-}" && -x "${CONDA_PREFIX}/bin/python" ]]; then
    python_bin="${CONDA_PREFIX}/bin/python"
elif [[ -n "${UV_PROJECT_ENVIRONMENT:-}" && -x "${UV_PROJECT_ENVIRONMENT}/bin/python" ]]; then
    python_bin="${UV_PROJECT_ENVIRONMENT}/bin/python"
elif [[ -x ".venv/bin/python" ]]; then
    python_bin="$PWD/.venv/bin/python"
elif command -v python3 >/dev/null 2>&1 && \
     python3 -c 'import sys; raise SystemExit(0 if sys.prefix != sys.base_prefix else 1)' 2>/dev/null; then
    python_bin="$(command -v python3)"
fi

if [[ -z "$python_bin" ]]; then
    cat >&2 <<'EOF'
No virtual environment was found.

Activate the environment you want Keyloom installed in, then rerun this script:
    source /path/to/venv/bin/activate
    ./scripts/install.sh

Or point it at that environment's interpreter explicitly:
    PYTHON=/path/to/venv/bin/python ./scripts/install.sh

Keyloom no longer creates .venv for you.
EOF
    exit 2
fi

# Allow PYTHON=python3 as well as a path.
if [[ "$python_bin" != */* ]]; then
    python_bin="$(command -v "$python_bin" || true)"
fi
if [[ -z "$python_bin" || ! -x "$python_bin" ]]; then
    printf 'Not an executable Python interpreter: %s\n' "${python_bin:-<empty>}" >&2
    exit 2
fi

# Never silently install into the distro Python's site-packages.
if ! "$python_bin" -c 'import sys; raise SystemExit(0 if sys.prefix != sys.base_prefix else 1)' 2>/dev/null; then
    if [[ "${KEYLOOM_ALLOW_SYSTEM_PYTHON:-}" != "1" ]]; then
        printf 'Refusing to install into the system Python: %s\n' "$python_bin" >&2
        printf 'Activate a virtual environment first, or set KEYLOOM_ALLOW_SYSTEM_PYTHON=1 to override.\n' >&2
        exit 2
    fi
fi

# uv-created environments usually have no pip, so prefer uv when it is available.
if command -v uv >/dev/null 2>&1; then
    uv pip install --python "$python_bin" -e .
else
    if ! "$python_bin" -m pip --version >/dev/null 2>&1; then
        "$python_bin" -m ensurepip --upgrade >/dev/null 2>&1 || true
    fi
    if ! "$python_bin" -m pip --version >/dev/null 2>&1; then
        printf 'pip is not available in %s. Install pip there, or install uv and rerun.\n' "$python_bin" >&2
        exit 2
    fi
    "$python_bin" -m pip install --upgrade pip
    "$python_bin" -m pip install -e .
fi

bin_dir="$(dirname "$python_bin")"
printf '\nKeyloom installed into %s\n' "$python_bin"
printf 'Try: %s --check examples/two_spots.loom\n' "$bin_dir/keyloom"
printf 'Run: %s --run examples/two_spots.loom\n' "$bin_dir/keyloom"
