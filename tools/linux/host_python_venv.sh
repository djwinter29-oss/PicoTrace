#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
PYTHON_PROJECT_DIR="$REPO_ROOT/host/python"
VENV_DIR=${1:-.venv}
PYTHON_BIN=${PYTHON_BIN:-python3}
FULL_VENV_DIR="$REPO_ROOT/$VENV_DIR"
VENV_PYTHON="$FULL_VENV_DIR/bin/python"

if ! command -v "$PYTHON_BIN" >/dev/null 2>&1; then
    printf 'Python launcher not found: %s\n' "$PYTHON_BIN" >&2
    printf 'Set PYTHON_BIN=/path/to/python3 if Python 3 is installed elsewhere.\n' >&2
    exit 1
fi

cd "$PYTHON_PROJECT_DIR"

if [ ! -x "$VENV_PYTHON" ]; then
    "$PYTHON_BIN" -m venv "$FULL_VENV_DIR"
fi

"$VENV_PYTHON" -m pip install --upgrade pip
"$VENV_PYTHON" -m pip install -r requirements.txt

printf 'Virtual environment ready at %s\n' "$FULL_VENV_DIR"
printf 'Activate with: source %s/bin/activate\n' "$FULL_VENV_DIR"
