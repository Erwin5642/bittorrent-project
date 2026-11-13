#!/usr/bin/env bash
# Cliente de linha de comando do servidor de rendezvous (só bash, sem nc).
#
# uso: rdv.sh <host:porta> <comando> [args]
#   register <porta> [ip]    registra/renova este Super Peer; imprime "OK ip:porta" e linhas "PEER ip:porta"
#   unregister <porta> [ip]  remove o registro
#   list                     lista os Super Peers registrados
#   ping                     testa o servidor (PONG)
#   whoami                   o endereço externo (ip:porta) com que o servidor vê esta conexão
#
# Saída: a resposta do servidor, linha a linha, sem o "END". Código de saída:
#   0 ok · 1 o servidor respondeu ERR · 2 erro de uso · 3 sem conexão/timeout
set -u

SERVER="${1:-}"
CMD="${2:-}"
[[ -n "$SERVER" && -n "$CMD" && "$SERVER" == *:* ]] || {
    sed -n '2,13p' "$0" >&2
    exit 2
}
# Vigia: o /dev/tcp do bash não tem prazo de conexão (host fora do ar = ~2 min de espera).
if [[ -z "${RDV_INNER:-}" ]]; then
    RDV_INNER=1 timeout "$(( ${RDV_TIMEOUT:-5} + 5 ))" bash "$0" "$@"
    rc=$?
    if [[ $rc -eq 124 ]]; then
        echo "ERR sem resposta de $SERVER (timeout)" >&2
        exit 3
    fi
    exit "$rc"
fi
HOST="${SERVER%:*}"
PORT="${SERVER##*:}"
TIMEOUT="${RDV_TIMEOUT:-5}"
shift 2

case "$CMD" in
register | unregister) [[ $# -ge 1 ]] || { echo "falta a porta" >&2; exit 2; }; REQ="${CMD^^} $*" ;;
list | ping | whoami) REQ="${CMD^^}" ;;
*) echo "comando desconhecido: $CMD" >&2; exit 2 ;;
esac

{ exec 3<>"/dev/tcp/$HOST/$PORT"; } 2>/dev/null || { echo "ERR sem conexao com $SERVER" >&2; exit 3; }
printf '%s\n' "$REQ" >&3

rc=3
got=0
while IFS= read -r -t "$TIMEOUT" line <&3; do
    line="${line%$'\r'}"
    got=1
    case "$line" in
    END) rc=0; break ;;
    ERR*) echo "$line" >&2; rc=1; break ;;
    OK | PONG | OK\ *) echo "$line"; rc=0 ;;
    *) echo "$line" ;;
    esac
done
exec 3>&- 3<&-
[[ $got -eq 1 ]] || echo "ERR sem resposta de $SERVER" >&2
exit "$rc"
