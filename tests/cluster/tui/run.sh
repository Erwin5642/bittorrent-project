#!/usr/bin/env bash
# Abre a TUI. Cria .venv na primeira execução (precisa de rede para o pip).
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ ! -x "$DIR/.venv/bin/python" ]]; then
    python3 -m venv "$DIR/.venv"
    "$DIR/.venv/bin/pip" install -q -r "$DIR/requirements.txt"
fi
exec "$DIR/.venv/bin/python" "$DIR/app.py" "$@"
