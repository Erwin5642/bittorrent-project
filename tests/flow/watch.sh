#!/usr/bin/env bash
# Abre uma sessão tmux: o roteiro num painel, cada log de processo noutro.
# O harness em si continua sendo tests/flow/run.sh.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LOG_DIR="$ROOT/tests/flow/logs"
SESSION="${FLOW_TMUX_SESSION:-bittorrent-flow}"

if ! command -v tmux >/dev/null 2>&1; then
    echo "tmux não encontrado" >&2
    exit 1
fi

if [[ -n "${TMUX:-}" ]]; then
    echo "Já está dentro do tmux. Saia desta sessão e rode de novo." >&2
    exit 1
fi

mkdir -p "$LOG_DIR"

client_logs=(
    upload-a.log upload-b.log upload-c.log upload-d.log upload-e.log
    state-5101.log state-5102.log state-5103.log
    lookup-1.log lookup-2.log
    download-a.log
)
for name in sp5101.log sp5102.log sp5103.log sp5102-rejoin.log "${client_logs[@]}"; do
    : > "$LOG_DIR/$name"
done

if tmux has-session -t "$SESSION" 2>/dev/null; then
    tmux kill-session -t "$SESSION"
fi

pane_cmd() {
    local title="$1"
    shift
    local cmd file
    cmd="echo $(printf '%q' "=== $title ==="); exec tail -n +1 -F --"
    for file in "$@"; do
        cmd+=" $(printf '%q' "$file")"
    done
    printf '%s' "$cmd"
}

# Primeiro o harness, no painel largo. Os tails nascem ao lado, ainda com
# largura sobrando, e o layout main-vertical empilha eles à direita.
tmux new-session -d -s "$SESSION" -n fluxo -x 160 -y 50 -c "$ROOT" \
    "FLOW_WATCH=1 bash tests/flow/run.sh; echo; echo \"[harness] encerrou com \$?\"; read"
tmux select-pane -t "$SESSION:" -T "harness"

add_tail() {
    local title="$1"
    shift
    tmux split-window -h -t "$SESSION:" -c "$ROOT" "$(pane_cmd "$title" "$@")"
    tmux select-pane -t "$SESSION:" -T "$title"
}

add_tail "sp5101" "$LOG_DIR/sp5101.log"
add_tail "sp5102" "$LOG_DIR/sp5102.log"
add_tail "sp5103" "$LOG_DIR/sp5103.log"
add_tail "sp5102-rejoin" "$LOG_DIR/sp5102-rejoin.log"
add_tail "clientes" "${client_logs[@]/#/$LOG_DIR/}"

tmux select-pane -t "$SESSION:fluxo.0"
tmux select-layout -t "$SESSION" main-vertical
tmux set-option -t "$SESSION" pane-border-status top
tmux set-option -t "$SESSION" pane-border-format ' #{pane_title} '

if [[ -t 1 ]]; then
    exec tmux attach -t "$SESSION"
fi

echo "Sessão $SESSION pronta. Anexe com: tmux attach -t $SESSION"
