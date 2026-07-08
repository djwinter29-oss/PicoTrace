#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
PYTHON_PROJECT_DIR="$REPO_ROOT/host/python"
VENV_DIR=${1:-host/python/.venv}
FULL_VENV_DIR="$REPO_ROOT/$VENV_DIR"

cd "$PYTHON_PROJECT_DIR"

if [ ! -d "$FULL_VENV_DIR" ]; then
    python3 -m venv "$FULL_VENV_DIR"
fi

"$FULL_VENV_DIR/bin/python" -m pip install --upgrade pip
"$FULL_VENV_DIR/bin/python" -m pip install -r requirements.txt

printf 'Virtual environment ready at %s\n' "$FULL_VENV_DIR"
printf 'Activate with: source %s/bin/activate\n' "$FULL_VENV_DIR"
