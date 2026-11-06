#!/usr/bin/env bash
# Funções do harness de fluxo. Cada processo tem o próprio arquivo de log.

source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"

FLOW_LOG="$PROJECT_ROOT/tests/flow/logs"
FLOW_OUT="$PROJECT_ROOT/tests/flow/out"
FLOW_CONF="$CONFIG_DIR/flow.conf"
LAST_PID=""

mkdir -p "$FLOW_LOG" "$FLOW_OUT"

say() { printf '[harness] %s\n' "$*"; }

die() {
    say "FALHA: $*"
    fail "$*"
    summary
    exit 1
}

start_node() {
    local port="$1"
    local logfile="$2"
    local tag="$3"

    : > "$logfile"
    say "subindo $tag na porta $port → $(basename "$logfile")"
    stdbuf -oL -eL "$NODE_BIN" --config "$FLOW_CONF" --port "$port" >>"$logfile" 2>&1 &
    LAST_PID=$!
    PIDS+=("$LAST_PID")
}

stop_pid() {
    local pid="$1"
    local who="${2:-pid $pid}"

    say "encerrando $who (kill -9 $pid)"
    kill -9 "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

wait_log() {
    local file="$1"
    local pattern="$2"
    local timeout="$3"
    local desc="$4"
    local hit

    say "esperando até ${timeout}s em $(basename "$file"): $desc"
    say "  padrão: $pattern"
    if ! wait_for_pattern "$file" "$pattern" "$timeout"; then
        die "tempo esgotado (${timeout}s) em $(basename "$file"): $desc. Padrão: $pattern"
    fi
    hit=$(grep -E "$pattern" "$file" | tail -n 1)
    say "apareceu em $(basename "$file"): $hit"
}

run_client() {
    local logfile="$1"
    local tag
    local rc

    shift
    tag=$(basename "$logfile" .log)
    : > "$logfile"
    say "cliente $tag: $CLIENT_BIN $*"
    "$CLIENT_BIN" "$@" >"$logfile" 2>&1
    rc=$?
    if [[ "$rc" -ne 0 ]]; then
        die "cliente saiu com $rc ($*) — ver $(basename "$logfile")"
    fi
    say "cliente $tag terminou com sucesso"
}

# PREPARE, N votos YES, LOG #<idx>, STORE <nome>, COMMIT, sem outro PREPARE no meio.
assert_round() {
    local file="$1"
    local name="$2"
    local expect_yes="$3"
    local idx="$4"
    local result

    say "conferindo 2PC de $name em $(basename "$file"): PREPARE, $expect_yes YES, LOG #$idx, COMMIT"
    result=$(awk -v name="$name" -v idx="$idx" -v expect="$expect_yes" '
        { lines[++n] = $0 }
        END {
            store = 0
            for (i = 1; i <= n; i++) {
                if (lines[i] == "STORE " name) {
                    store = i
                }
            }
            if (!store) {
                print "sem STORE " name
                exit
            }
            if (store == 1 || lines[store - 1] != "LOG #" idx) {
                print "antes de STORE " name " veio: " lines[store - 1]
                exit
            }
            prepare = 0
            for (i = store; i >= 1; i--) {
                if (lines[i] == "PREPARE") {
                    prepare = i
                    break
                }
            }
            if (!prepare) {
                print "sem PREPARE antes de " name
                exit
            }
            for (i = prepare + 1; i < store; i++) {
                if (lines[i] == "PREPARE") {
                    print "PREPARE extra antes de " name
                    exit
                }
            }
            commit = 0
            for (i = store; i <= n; i++) {
                if (lines[i] == "COMMIT" || lines[i] == "ABORT") {
                    commit = i
                    break
                }
            }
            if (!commit || lines[commit] != "COMMIT") {
                print "sem COMMIT depois de " name
                exit
            }
            yes = 0
            for (i = prepare + 1; i < commit; i++) {
                if (lines[i] == "YES") {
                    yes++
                }
            }
            if (yes != expect + 0) {
                print "YES=" yes " esperado " expect " em " name
                exit
            }
            print "ok"
        }
    ' "$file")
    if [[ "$result" != "ok" ]]; then
        die "$(basename "$file"): $result"
    fi
    say "2PC de $name ok em $(basename "$file")"
}

# LOG #<n> imediatamente seguido de STORE <nome>, na ordem dos nomes.
assert_log_sequence() {
    local file="$1"
    shift
    local idx=1
    local name
    local result

    say "conferindo a sequência do log em $(basename "$file"): $*"
    for name in "$@"; do
        result=$(awk -v name="$name" -v idx="$idx" '
            prev == "LOG #" idx && $0 == "STORE " name { found = 1 }
            { prev = $0 }
            END { if (!found) print "sem LOG #" idx " / STORE " name }
        ' "$file")
        if [[ -n "$result" ]]; then
            die "$(basename "$file"): $result"
        fi
        say "  $(basename "$file") tem LOG #$idx STORE $name"
        idx=$((idx + 1))
    done
}

assert_lines_in_order() {
    local file="$1"
    shift
    local missing

    say "conferindo a ordem em $(basename "$file"): $*"
    missing=$(awk -v list="$(printf '%s\n' "$@")" '
        BEGIN {
            n = split(list, pats, "\n")
            if (n > 0 && pats[n] == "") {
                n--
            }
            p = 1
        }
        p <= n && $0 == pats[p] { p++ }
        END {
            if (p <= n) {
                print pats[p]
            }
        }
    ' "$file")
    if [[ -n "$missing" ]]; then
        die "$(basename "$file") não teve a linha '$missing' depois das anteriores"
    fi
    say "ordem ok em $(basename "$file")"
}

assert_exact_absent() {
    local file="$1"
    local line="$2"

    say "confirmando que $(basename "$file") não tem a linha '$line'"
    if grep -qx -F "$line" "$file"; then
        die "$(basename "$file") contém a linha '$line'"
    fi
    say "$(basename "$file") não tem '$line'"
}

assert_name_absent() {
    local file="$1"
    local name="$2"

    say "confirmando que $(basename "$file") não aplicou STORE $name"
    if grep -qx -F "STORE $name" "$file"; then
        die "$(basename "$file") contém STORE $name"
    fi
    say "$(basename "$file") não tem STORE $name"
}

assert_state() {
    local port="$1"
    local index="$2"
    local objects="$3"
    shift 3
    local logfile="$FLOW_LOG/state-${port}.log"
    local name

    say "consultando state na porta $port: espero LogIndex $index, Objects: $objects, nomes: $*"
    run_client "$logfile" --cmd state --host 127.0.0.1 --port "$port"
    if ! grep -qx "LogIndex $index" "$logfile"; then
        die "state :$port sem LogIndex $index — ver $(basename "$logfile")"
    fi
    if ! grep -qx "Objects: $objects" "$logfile"; then
        die "state :$port sem Objects: $objects — ver $(basename "$logfile")"
    fi
    for name in "$@"; do
        if ! grep -q "^${name} v" "$logfile"; then
            die "state :$port sem $name — ver $(basename "$logfile")"
        fi
    done
    ok "state :$port LogIndex $index, $objects objetos"
}
