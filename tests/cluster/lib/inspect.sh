#!/usr/bin/env bash
# Inspeção dos Super Peers: membership, Chord, coordenador, metadados, estado do SMR e eventos.
# Tudo sai de duas fontes que o harness já tem: os logs espelhados em tests/cluster/logs
# (membros, sucessor, fingers, eleição, metadados, 2PC, IST) e o GET_STATE ao vivo (state).
# O Super Peer não registra o predecessor, então ele não aparece aqui.

# Nomes pedidos (padrão: todos os Super Peers). Confere se existem e se têm log.
ins_names() {
    local n
    INS_NAMES=()
    if [[ $# -eq 0 ]]; then
        INS_NAMES=("${NODE_LIST[@]}")
        return 0
    fi
    for n in "$@"; do
        [[ -n "${NODE_HOST[$n]:-}" ]] || { echo "erro: Super Peer desconhecido: $n" >&2; return 1; }
        INS_NAMES+=("$n")
    done
}

# Log do nó sem o carimbo de hora. Falha (com aviso) se o log não existe.
ins_cat() {
    local f
    f="$(node_log "$1")"
    [[ -e "$f" ]] || { echo "sem log de $1 (o cluster está no ar? rode: cluster.sh up)" >&2; return 1; }
    sed -E 's/^[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3} //' "$f"
}

# NodeID (hex) do nó, lido do log; vazio se o nó ainda não imprimiu.
ins_nodeid() { ins_cat "$1" 2>/dev/null | awk '/^NodeID: /{ id = $2 } END { print id }'; }

# Nome do Super Peer com esse NodeID (ou os 12 primeiros dígitos, se for desconhecido).
ins_name_of_id() {
    local id="$1" n
    for n in "${NODE_LIST[@]}"; do
        [[ "${INS_ID[$n]:-}" == "$id" ]] && { printf '%s' "$n"; return 0; }
    done
    printf '%s' "${id:0:12}"
}

# Nome do Super Peer de um ip:porta (ou o próprio ip:porta).
ins_name_of_ep() { node_by_endpoint "$1" || printf '%s' "$1"; }

# Carrega INS_ID[nome] com o NodeID de todos os Super Peers.
ins_load_ids() {
    local n
    declare -gA INS_ID=()
    for n in "${NODE_LIST[@]}"; do INS_ID[$n]="$(ins_nodeid "$n")"; done
}

# Estado de cada membro segundo a tabela do nó: o último visto no log, em ordem de tempo.
# Preenche INS_MS[ip:porta]=ALIVE|SUSPECT|FAILED|REMOVED.
ins_load_members() {
    local ep st
    declare -gA INS_MS=()
    while read -r ep st; do
        INS_MS[$ep]="$st"
    done < <(ins_cat "$1" 2>/dev/null | awk '
        $1 == "SUPERPEER" || $1 == "PEER" { s[$2 ":" $3] = $5; next }
        $1 == "SUSPECT" || $1 == "FAILED" { s[$3] = $1 }
        END { for (k in s) print k, s[k] }')
}

# --- members ---------------------------------------------------------------

# Matriz observador x membro: o que a tabela de cada Super Peer diz de cada membro.
cmd_members() {
    ins_names "$@" || return 1
    local obs m cell
    printf '%-9s' 'OBSERVA'
    for m in "${NODE_LIST[@]}"; do printf '%-9s' "$m"; done
    echo
    for obs in "${INS_NAMES[@]}"; do
        ins_load_members "$obs"
        printf '%-9s' "$obs"
        for m in "${NODE_LIST[@]}"; do
            cell="${INS_MS[$(node_endpoint "$m")]:--}"
            printf '%-9s' "$cell"
        done
        echo
    done
    echo
    echo "Fonte: último SUPERPEER (tabela impressa na partida e a cada JOIN) e eventos SUSPECT/FAILED do log de cada observador."
    echo "'-' = o nó nunca imprimiu esse membro. As atualizações por gossip não vão para o log, então só o SUSPECT/FAILED é acompanhado depois da partida."
}

# --- chord -----------------------------------------------------------------

# Anel pelo NodeID, com o sucessor que cada nó reporta e o que a ordem do anel espera.
# Com --fingers, também as entradas da finger table que o nó registrou.
cmd_chord() {
    local fingers=0 a
    local -a want=()
    for a in "$@"; do
        case "$a" in
        --fingers) fingers=1 ;;
        *) want+=("$a") ;;
        esac
    done
    ins_names "${want[@]}" || return 1
    ins_load_ids

    local -a ring=()
    mapfile -t ring < <(for n in "${NODE_LIST[@]}"; do [[ -n "${INS_ID[$n]}" ]] && echo "${INS_ID[$n]} $n"; done | sort | awk '{ print $2 }')
    if [[ ${#ring[@]} -eq 0 ]]; then
        echo "nenhum Super Peer imprimiu o NodeID ainda" >&2
        return 1
    fi

    printf '%-4s %-8s %-14s %-9s %-9s %s\n' POS NOME NODEID SUCESSOR ESPERADO STATUS
    local i name succ expect pos n j cand
    for name in "${ring[@]}"; do
        [[ " ${INS_NAMES[*]} " == *" $name "* ]] || continue
        ins_load_members "$name"
        pos=0
        for i in "${!ring[@]}"; do [[ "${ring[$i]}" == "$name" ]] && pos=$i; done
        # esperado: o próximo no anel que o próprio nó não dá como FAILED/REMOVED
        expect="$name"
        for ((j = 1; j < ${#ring[@]}; j++)); do
            cand="${ring[$(((pos + j) % ${#ring[@]}))]}"
            case "${INS_MS[$(node_endpoint "$cand")]:-ALIVE}" in
            FAILED | REMOVED) continue ;;
            esac
            expect="$cand"
            break
        done
        succ="$(ins_cat "$name" 2>/dev/null | awk '/^Successor -> /{ s = $3 } END { print s }')"
        succ="${succ:+$(ins_name_of_ep "$succ")}"
        printf '%-4s %-8s %-14s %-9s %-9s %s\n' "$pos" "$name" "${INS_ID[$name]:0:12}…" "${succ:--}" "$expect" \
            "$([[ -z "$succ" ]] && echo 'sem sucessor no log' || { [[ "$succ" == "$expect" ]] && echo ok || echo DIVERGE; })"
    done
    echo
    echo "Ordem do anel = NodeID crescente. ESPERADO ignora quem o nó já marcou FAILED. O predecessor não é registrado pelo Super Peer."

    if ((fingers)); then
        for name in "${INS_NAMES[@]}"; do
            echo
            echo "Finger table de $name (só as entradas registradas no log; as demais apontam para o próprio nó):"
            ins_cat "$name" 2>/dev/null | awk '
                match($0, /^Finger\[[0-9]+\] -> /) { i = substr($0, 8, index($0, "]") - 8); f[i] = $NF }
                END { for (i in f) print i, f[i] }' | sort -n | {
                    first=""; last=""; cur=""
                    while read -r idx ep; do
                        if [[ "$ep" == "$cur" && "$idx" -eq $((last + 1)) ]]; then
                            last="$idx"
                            continue
                        fi
                        [[ -n "$cur" ]] && printf '  finger[%s] -> %s (%s)\n' "$([[ $first == $last ]] && echo "$first" || echo "$first..$last")" "$(ins_name_of_ep "$cur")" "$cur"
                        first="$idx"; last="$idx"; cur="$ep"
                    done
                    [[ -n "$cur" ]] && printf '  finger[%s] -> %s (%s)\n' "$([[ $first == $last ]] && echo "$first" || echo "$first..$last")" "$(ins_name_of_ep "$cur")" "$cur"
                }
        done
    fi
}

# --- coordinator -----------------------------------------------------------

# Quem cada Super Peer reconhece como coordenador, e se todos concordam.
cmd_coordinator() {
    ins_names "$@" || return 1
    ins_load_ids
    local name id elections coord agreed="" mixed=0 expected
    printf '%-8s %-14s %-10s %s\n' NOME COORDENADOR ELEICOES INICIOU_ULTIMA
    for name in "${INS_NAMES[@]}"; do
        id="$(ins_cat "$name" 2>/dev/null | awk '/^COORDINATOR /{ c = $2 } END { print c }')"
        elections="$(ins_cat "$name" 2>/dev/null | grep -c '^ELECTION started by ' || true)"
        coord="${id:+$(ins_name_of_id "$id")}"
        printf '%-8s %-14s %-10s %s\n' "$name" "${coord:--}" "$elections" \
            "$(ins_cat "$name" 2>/dev/null | awk '/^ELECTION started by /{ e = $4 } END { print substr(e, 1, 12) }')"
        [[ -n "$coord" ]] || continue
        [[ -z "$agreed" ]] && agreed="$coord"
        [[ "$agreed" == "$coord" ]] || mixed=1
    done
    expected="$(node_highest)"
    echo
    if [[ -z "$agreed" ]]; then
        echo "nenhum coordenador anunciado ainda"
    elif ((mixed)); then
        echo "DIVERGÊNCIA: os Super Peers não concordam sobre o coordenador"
    else
        echo "todos reconhecem $agreed (esperado pela maior prioridade, com todos vivos: $expected)"
    fi
}

# --- state -----------------------------------------------------------------

# Estado ao vivo (GET_STATE) de cada Super Peer: LogIndex, objetos, cache LFU.
# Com --files, a matriz arquivo x Super Peer com a versão de cada um.
cmd_state() {
    local files=0 a
    local -a want=()
    for a in "$@"; do
        case "$a" in
        --files) files=1 ;;
        *) want+=("$a") ;;
        esac
    done
    ins_names "${want[@]}" || return 1

    local dir="$STATE/inspect" name
    rm -rf "$dir"
    mkdir -p "$dir"
    local -a pids=()
    for name in "${INS_NAMES[@]}"; do
        ( node_state_text "$name" >"$dir/$name" 2>/dev/null || : >"$dir/$name.fail" ) &
        pids+=("$!")
    done
    wait "${pids[@]}" 2>/dev/null

    printf '%-8s %-9s %-8s %-8s %-8s %-10s %s\n' NOME LOGINDEX OBJETOS HITS MISSES EVICTIONS HIT_RATE
    local idx obj hits miss ev rate silent=""
    local -A seen_idx=() seen_obj=()
    for name in "${INS_NAMES[@]}"; do
        if [[ -e "$dir/$name.fail" || ! -s "$dir/$name" ]]; then
            printf '%-8s %s\n' "$name" "sem resposta"
            silent+="$name "
            continue
        fi
        idx="$(awk '$1 == "LogIndex" { print $2; exit }' "$dir/$name")"
        obj="$(awk '$1 == "Objects:" { print $2; exit }' "$dir/$name")"
        read -r hits miss ev rate < <(awk '$1 == "Cache" { print $3, $5, $7, $10 }' "$dir/$name")
        printf '%-8s %-9s %-8s %-8s %-8s %-10s %s\n' "$name" "$idx" "$obj" "${hits:--}" "${miss:--}" "${ev:--}" "${rate:--}"
        seen_idx[$idx]=1
        seen_obj[$obj]=1
    done
    echo
    if [[ ${#seen_idx[@]} -eq 0 ]]; then
        echo "nenhum Super Peer respondeu (o cluster está no ar? rode: cluster.sh up)"
        return 1
    elif [[ ${#seen_idx[@]} -le 1 && ${#seen_obj[@]} -le 1 ]]; then
        echo "consistente: mesmo LogIndex e mesmo número de objetos nos Super Peers que responderam${silent:+ (sem resposta: ${silent% })}"
    else
        echo "DIVERGÊNCIA: LogIndex ou número de objetos diferem entre os Super Peers (SMR ainda propagando, ou nó atrasado)"
    fi

    if ((files)); then
        echo
        printf '%-28s' ARQUIVO
        for name in "${INS_NAMES[@]}"; do printf '%-8s' "$name"; done
        echo
        local f v
        while read -r f; do
            printf '%-28s' "$f"
            for name in "${INS_NAMES[@]}"; do
                v="$(awk -v f="$f" '$1 == f && $2 ~ /^v[0-9]+$/ { print $2; exit }' "$dir/$name" 2>/dev/null)"
                printf '%-8s' "${v:--}"
            done
            echo
        done < <(cat "$dir"/SP* 2>/dev/null | awk '$2 ~ /^v[0-9]+$/ { print $1 }' | sort -u)
    fi
}

# --- metadata --------------------------------------------------------------

# Metadados (arquivo, tamanho, ObjectID, chunks) que cada Super Peer registrou no log.
# Filtro opcional: parte do nome do arquivo.
cmd_metadata() {
    local filter="${1:-}" name
    ins_names || return 1
    local -A info=() holders=()
    local rec file
    for name in "${INS_NAMES[@]}"; do
        ins_cat "$name" 2>/dev/null | awk -v n="$name" '
            /^File: / { if (f != "") flush(); f = substr($0, 7); size = ""; oid = ""; cc = ""; ch = ""; next }
            f != "" && /^Size: /   { size = $2; next }
            f != "" && /^ObjectID: / { oid = $2; next }
            f != "" && /^Chunks: / { cc = $2; next }
            f != "" && /^Chunk [0-9]+: / { ch = ch (ch == "" ? "" : ",") $3; next }
            function flush() { printf "%s\t%s\t%s\t%s\t%s\t%s\n", f, size, oid, cc, ch, n; f = "" }
            END { if (f != "") flush() }' |
            while IFS=$'\t' read -r file size oid cc ch who; do
                [[ -z "$filter" || "$file" == *"$filter"* ]] || continue
                printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$file" "$size" "$oid" "$cc" "$ch" "$who"
            done
    done | sort -t$'\t' -k1,1 -k6,6 | awk -F'\t' '
        { rec[$1] = $2 "\t" $3 "\t" $4 "\t" $5; who[$1] = who[$1] (who[$1] == "" ? "" : " ") $6 }
        END {
            for (f in rec) {
                split(rec[f], r, "\t")
                printf "Arquivo   %s\nTamanho   %s bytes\nObjectID  %s\nChunks    %s\n", f, r[1], r[2], r[3]
                n = split(r[4], c, ",")
                for (i = 1; i <= n; i++) printf "  chunk %d  %s\n", i - 1, c[i]
                printf "Registrado no log de: %s\n\n", who[f]
            }
        }'
}

# --- lookup ----------------------------------------------------------------

# Caminho Chord até o dono de um objeto, com os nomes dos Super Peers.
# <nome|object-id> [SP]; com o nome, o ObjectID sai do log de metadados.
cmd_lookup() {
    [[ $# -ge 1 ]] || { echo "uso: lookup <nome|object-id> [SP]" >&2; return 1; }
    local key="$1" via="${2:-${NODE_LIST[0]}}" oid h abs line ep
    [[ -n "${NODE_HOST[$via]:-}" ]] || { echo "erro: Super Peer desconhecido: $via" >&2; return 1; }
    if [[ "$key" =~ ^[0-9a-fA-F]{64}$ ]]; then
        oid="$key"
    else
        oid="$(cmd_metadata "$key" 2>/dev/null | awk -v f="$key" '$1 == "Arquivo" { ok = ($2 == f) } ok && $1 == "ObjectID" { print $2; exit }')"
        [[ -n "$oid" ]] || { echo "ObjectID de '$key' não está nos logs (nome exato; ou passe o ObjectID de 64 dígitos)" >&2; return 1; }
    fi
    h="${NODE_HOST[$via]}"
    abs="$(host_abs "$h")" || return 1
    echo "consultando $via ($(node_endpoint "$via")) pelo ObjectID $oid"
    REXEC_TIMEOUT=20 rexec "$h" "'$abs/bin/client' --cmd lookup --object-id '$oid' --host '$(node_ip "$via")' --port ${NODE_PORT[$via]}" |
        while IFS= read -r line; do
            if [[ "$line" =~ ^[0-9.]+:[0-9]+$ ]]; then
                printf '%s  (%s)\n' "$line" "$(ins_name_of_ep "$line")"
            elif [[ "$line" == "Owner: "* ]]; then
                ep="${line#Owner: }"
                printf 'Owner: %s  (%s)\n' "$ep" "$(ins_name_of_ep "$ep")"
            else
                printf '%s\n' "$line"
            fi
        done
}

# --- events ----------------------------------------------------------------

# Últimos eventos de um Super Peer, por categoria. Sem categoria: tudo, menos o ruído
# periódico (HEARTBEAT, GOSSIP, PING). 'all' inclui o ruído.
# uso: events <SP> [membership|election|chord|2pc|ist|lfu|log|metadata|all|regex] [N]
cmd_events() {
    [[ $# -ge 1 ]] || { echo "uso: events <SP> [categoria|regex] [N]  (categorias: membership election chord 2pc ist lfu log metadata all)" >&2; return 1; }
    local name="$1" cat="${2:-}" n="${3:-40}" re
    ins_names "$name" || return 1
    case "$cat" in
    membership) re='^(SUSPECT|FAILED|SUPERPEER|PEER) ' ;;
    election) re='^(ELECTION|COORDINATOR|highest NodeID)' ;;
    chord) re='^(Successor|Finger\[)' ;;
    2pc) re='^(UPLOAD|PREPARE|YES|NO|COMMIT|ABORT|settled)' ;;
    ist) re='^(IST|checkpoint|version |versions missing|receive logs|STATE_TRANSFER|READY|SMR log resumed|resume operations|log hole)' ;;
    lfu) re='^(cache hit|cache miss|eviction|LFU frequency)' ;;
    log) re='^(LOG #|LogIndex|SMR log|settled|checkpoint)' ;;
    metadata) re='^(File:|Size:|ObjectID:|Chunks:|Chunk [0-9]+:)' ;;
    all) re='' ;;
    '') re='' ;;
    *) re="$cat" ;;
    esac
    if [[ -n "$re" ]]; then
        # o carimbo fica: aqui ele é o que interessa
        grep -E "^[0-9:.]{12} ${re#^}" "$(node_log "$name")" | tail -n "$n"
    elif [[ "$cat" == all ]]; then
        tail -n "$n" "$(node_log "$name")"
    else
        grep -Ev '^[0-9:.]{12} (TX HEARTBEAT|TX GOSSIP|RX PING|RX HEARTBEAT|RX GOSSIP|(SUPERPEER|PEER) |Finger\[)' "$(node_log "$name")" | tail -n "$n"
    fi
}

# --- overview --------------------------------------------------------------

# Uma linha por Super Peer, só com o que os logs e o ssh já dizem (nada de GET_STATE).
cmd_overview() {
    ins_names "$@" || return 1
    ins_load_ids
    local name proc succ coord id idx alive susp fail st
    printf '%-8s %-6s %-14s %-9s %-12s %-8s %s\n' NOME PROC NODEID SUCESSOR COORDENADOR LOGINDEX 'MEMBROS(A/S/F)'
    for name in "${INS_NAMES[@]}"; do
        proc="$(rrun "${NODE_HOST[$name]}" alive "$name" 2>/dev/null | awk '{ print $2 }')"
        succ="$(ins_cat "$name" 2>/dev/null | awk '/^Successor -> /{ s = $3 } END { print s }')"
        succ="${succ:+$(ins_name_of_ep "$succ")}"
        id="$(ins_cat "$name" 2>/dev/null | awk '/^COORDINATOR /{ c = $2 } END { print c }')"
        coord="${id:+$(ins_name_of_id "$id")}"
        idx="$(ins_cat "$name" 2>/dev/null | awk 'match($0, /LogIndex [0-9]+/) { v = substr($0, RSTART + 9, RLENGTH - 9) } END { print v }')"
        ins_load_members "$name"
        alive=0; susp=0; fail=0
        for st in "${INS_MS[@]}"; do
            case "$st" in
            ALIVE) alive=$((alive + 1)) ;;
            SUSPECT) susp=$((susp + 1)) ;;
            FAILED | REMOVED) fail=$((fail + 1)) ;;
            esac
        done
        printf '%-8s %-6s %-14s %-9s %-12s %-8s %s\n' "$name" "${proc:-?}" "${INS_ID[$name]:+${INS_ID[$name]:0:12}…}" \
            "${succ:--}" "${coord:--}" "${idx:--}" "$alive/$susp/$fail"
    done
}
