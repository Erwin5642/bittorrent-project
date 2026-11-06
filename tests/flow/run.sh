#!/usr/bin/env bash
# Roteiro do item 10: eleição, uploads A–C, queda do SP2, uploads D–E,
# IST na volta, LFU no SP1 e download byte a byte. Um arquivo de log por processo.

source "$(dirname "$0")/lib.sh"

section "Fluxo — eleição, 2PC, IST, LFU e download"
require_bin "$NODE_BIN" || exit 1
require_bin "$CLIENT_BIN" || exit 1

say "logs em $FLOW_LOG"
say "arquivos baixados em $FLOW_OUT"
if [[ -z "${FLOW_WATCH:-}" ]]; then
    say "este terminal fica só com o roteiro. Painéis: bash tests/flow/watch.sh"
fi

"$TEST_ROOT/generate_fixtures.sh" >/dev/null
say "fixtures prontas; montando documento_a.pdf … documento_e.pdf"
mkdir -p "$FLOW_OUT/files"
for letter in a b c d e; do
    cp "$DATA_DIR/small.txt" "$FLOW_OUT/files/documento_${letter}.pdf"
    printf '\n%s\n' "$letter" >> "$FLOW_OUT/files/documento_${letter}.pdf"
done

LOG1="$FLOW_LOG/sp5101.log"
LOG2="$FLOW_LOG/sp5102.log"
LOG3="$FLOW_LOG/sp5103.log"
LOG2_REJOIN="$FLOW_LOG/sp5102-rejoin.log"
NAMES_ABC=(documento_a.pdf documento_b.pdf documento_c.pdf)
NAMES_ALL=(documento_a.pdf documento_b.pdf documento_c.pdf documento_d.pdf documento_e.pdf)

say "fase 1: subir SP1, SP2 e SP3 e esperar o SP3 vencer a eleição"
start_node 5101 "$LOG1" sp5101
start_node 5102 "$LOG2" sp5102
SP2_PID=$LAST_PID
start_node 5103 "$LOG3" sp5103

wait_log "$LOG3" '^highest NodeID wins$' 30 "SP3 vencer a eleição"
COORD_HEX=$(awk '/^highest NodeID wins$/{getline; if ($1=="COORDINATOR"){print $2; exit}}' "$LOG3")
if [[ -z "$COORD_HEX" ]]; then
    die "SP3 não imprimiu COORDINATOR depois de highest NodeID wins"
fi
wait_log "$LOG1" "^COORDINATOR ${COORD_HEX}$" 20 "SP1 registrar o coordenador $COORD_HEX"
wait_log "$LOG2" "^COORDINATOR ${COORD_HEX}$" 20 "SP2 registrar o coordenador $COORD_HEX"
wait_log "$LOG1" '^Successor -> ' 15 "SP1 publicar o sucessor"
wait_log "$LOG2" '^Successor -> ' 15 "SP2 publicar o sucessor"
wait_log "$LOG3" '^Successor -> ' 15 "SP3 publicar o sucessor"
ok "eleição: coordenador $COORD_HEX e anel com sucessor"

upload_at() {
    local letter="$1"
    run_client "$FLOW_LOG/upload-${letter}.log" \
        --cmd upload --file "$FLOW_OUT/files/documento_${letter}.pdf" \
        --host 127.0.0.1 --port 5101
}

say "fase 2: upload de A, B e C na porta 5101, com SP1, SP2 e SP3 vivos"
upload_at a
upload_at b
upload_at c
assert_round "$LOG3" documento_a.pdf 2 1
assert_round "$LOG3" documento_b.pdf 2 2
assert_round "$LOG3" documento_c.pdf 2 3
assert_log_sequence "$LOG1" "${NAMES_ABC[@]}"
assert_log_sequence "$LOG2" "${NAMES_ABC[@]}"
ok "2PC de A, B e C com dois YES e o mesmo log nos três nós"
assert_state 5101 3 3 "${NAMES_ABC[@]}"
assert_state 5102 3 3 "${NAMES_ABC[@]}"
assert_state 5103 3 3 "${NAMES_ABC[@]}"

say "fase 3: derrubar o SP2 e esperar o coordenador marcar SUSPECT ou FAILED"
stop_pid "$SP2_PID" "SP2 (porta 5102)"
wait_log "$LOG3" '^(SUSPECT|FAILED) .*127\.0\.0\.1:5102$' 25 \
    "SP3 marcar o SP2 como SUSPECT ou FAILED"
ok "SP2 caiu e o coordenador deixou de contá-lo como vivo"

say "fase 4: upload de D e E enquanto o SP2 está fora. O 2PC deve ter um YES só"
upload_at d
upload_at e
assert_round "$LOG3" documento_d.pdf 1 4
assert_round "$LOG3" documento_e.pdf 1 5
assert_name_absent "$LOG2" documento_d.pdf
assert_name_absent "$LOG2" documento_e.pdf
ok "D e E com um YES; o log da primeira vida do SP2 não os tem"
assert_state 5101 5 5 "${NAMES_ALL[@]}"
assert_state 5103 5 5 "${NAMES_ALL[@]}"

say "fase 5: SP2 volta. O log novo é sp5102-rejoin.log e deve trazer A até E por IST"
start_node 5102 "$LOG2_REJOIN" sp5102-rejoin
wait_log "$LOG2_REJOIN" '^version 0 -> 5$' 40 "IST do SP2 anunciar version 0 -> 5"
assert_lines_in_order "$LOG2_REJOIN" \
    "IST" \
    "version 0 -> 5" \
    "STATE_TRANSFER" \
    "LOG #1" \
    "STORE documento_a.pdf" \
    "LOG #2" \
    "STORE documento_b.pdf" \
    "LOG #3" \
    "STORE documento_c.pdf" \
    "LOG #4" \
    "STORE documento_d.pdf" \
    "LOG #5" \
    "STORE documento_e.pdf"
ok "IST do SP2 trouxe A até E, com STATE_TRANSFER"
assert_state 5102 5 5 "${NAMES_ALL[@]}"

say "fase 6: duas consultas de A só na porta 5101. A primeira é miss, a segunda é hit"
run_client "$FLOW_LOG/lookup-1.log" \
    --cmd lookup --name documento_a.pdf --host 127.0.0.1 --port 5101
run_client "$FLOW_LOG/lookup-2.log" \
    --cmd lookup --name documento_a.pdf --host 127.0.0.1 --port 5101
assert_lines_in_order "$LOG1" \
    "cache miss" \
    "LFU frequency 1" \
    "cache hit" \
    "LFU frequency 2"
assert_exact_absent "$LOG2" "cache hit"
assert_exact_absent "$LOG3" "cache hit"
assert_exact_absent "$LOG2_REJOIN" "cache hit"
ok "LFU no SP1: miss e frequência 1, depois hit e frequência 2"

say "fase 7: download de A na porta 5101 e comparação byte a byte com o arquivo enviado"
run_client "$FLOW_LOG/download-a.log" \
    --cmd download --name documento_a.pdf --output "$FLOW_OUT/documento_a.out" \
    --host 127.0.0.1 --port 5101
if ! grep -qx "Download completed" "$FLOW_LOG/download-a.log"; then
    die "download-a.log sem Download completed"
fi
if ! grep -qx "SHA-256 verified" "$FLOW_LOG/download-a.log"; then
    die "download-a.log sem SHA-256 verified"
fi
if ! cmp -s "$FLOW_OUT/files/documento_a.pdf" "$FLOW_OUT/documento_a.out"; then
    die "download de documento_a.pdf difere do arquivo enviado"
fi
ok "download de A na porta 5101 é idêntico ao enviado"

summary
