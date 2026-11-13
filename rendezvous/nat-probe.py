#!/usr/bin/env python3
"""Teste de hole punching TCP entre duas máquinas, usando o servidor de rendezvous.

Cada máquina roda o script com um rótulo diferente, ao mesmo tempo:

    python3 nat-probe.py 13.184.245.205:7000 ana
    python3 nat-probe.py 13.184.245.205:7000 beto

Como funciona: o script escolhe uma porta local P e, SEMPRE a partir dela, (1) pergunta ao
rendezvous qual endereço externo o NAT deu a P, (2) descobre o endereço externo do outro e
(3) tenta conectar nele a partir de P, enquanto escuta em P. Os dois lados fazendo isso ao
mesmo tempo é o "simultaneous open" do TCP. Só usa a biblioteca padrão do Python 3.

Saída: "PUNCH OK" se uma conexão TCP direta foi aberta e as duas pontas trocaram uma mensagem.
Código de saída: 0 ok, 1 falhou, 2 uso errado.
"""

from __future__ import annotations

import argparse
import errno
import socket
import sys
import threading
import time


def make_socket(port: int = 0) -> socket.socket:
    """Socket TCP que pode compartilhar a porta local (escuta + conexões de saída)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    s.bind(("0.0.0.0", port))
    return s


def ask_rendezvous(server: tuple[str, int], port: int, line: str, timeout: float = 5.0) -> list[str]:
    """Manda um comando ao rendezvous A PARTIR da porta local `port` e devolve as linhas até END."""
    s = make_socket(port)
    s.settimeout(timeout)
    try:
        s.connect(server)
        s.sendall((line + "\n").encode())
        buf = b""
        while b"END\n" not in buf and not buf.startswith(b"ERR") and not buf.startswith(b"PONG"):
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        return [l for l in buf.decode(errors="replace").splitlines() if l and l != "END"]
    finally:
        s.close()


def parse_addr(text: str) -> tuple[str, int]:
    host, _, port = text.rpartition(":")
    return host, int(port)


def log(msg: str) -> None:
    print(f"{time.strftime('%H:%M:%S')} {msg}", flush=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rendezvous", help="host:porta do servidor de rendezvous")
    ap.add_argument("label", help="nome desta máquina (diferente em cada ponta)")
    ap.add_argument("--timeout", type=float, default=90, help="segundos até desistir (padrão 90)")
    ap.add_argument("--local-port", type=int, default=0, help="porta local P (padrão: uma livre)")
    args = ap.parse_args()
    try:
        server = parse_addr(args.rendezvous)
        server = (socket.gethostbyname(server[0]), server[1])
    except (ValueError, OSError) as exc:
        print(f"rendezvous inválido: {exc}", file=sys.stderr)
        return 2

    # Reserva a porta P e deixa uma escuta nela: o SYN do outro lado pode chegar antes do nosso.
    listener = make_socket(args.local_port)
    listener.listen(8)
    port = listener.getsockname()[1]
    log(f"porta local P = {port}")

    done = threading.Event()
    result: dict = {}

    def finish(conn: socket.socket, how: str) -> None:
        if done.is_set():
            conn.close()
            return
        result["conn"], result["how"] = conn, how
        done.set()

    def accept_loop() -> None:
        listener.settimeout(0.5)
        while not done.is_set():
            try:
                conn, addr = listener.accept()
            except (socket.timeout, OSError):
                continue
            log(f"conexão ACEITA de {addr[0]}:{addr[1]}")
            finish(conn, f"aceita de {addr[0]}:{addr[1]}")

    def connect_loop(peer: tuple[str, int]) -> None:
        attempts = 0
        while not done.is_set():
            attempts += 1
            s = make_socket(port)
            s.settimeout(2.0)
            try:
                s.connect(peer)
            except OSError as exc:
                s.close()
                result["last_error"] = errno.errorcode.get(exc.errno or 0, str(exc))
                time.sleep(0.3)
                continue
            log(f"conexão ABERTA para {peer[0]}:{peer[1]} (tentativa {attempts})")
            finish(s, f"aberta para {peer[0]}:{peer[1]} na tentativa {attempts}")
            return
        result["attempts"] = attempts

    threading.Thread(target=accept_loop, daemon=True).start()

    my_external: str | None = None
    punching: set[tuple[str, int]] = set()
    deadline = time.time() + args.timeout
    while not done.is_set() and time.time() < deadline:
        try:
            lines = ask_rendezvous(server, port, f"PROBE {args.label}")
        except OSError as exc:
            log(f"rendezvous inacessível: {exc}")
            time.sleep(2)
            continue
        if not lines or not lines[0].startswith("OK "):
            log(f"resposta inesperada do rendezvous: {lines}")
            time.sleep(2)
            continue
        external = lines[0][3:]
        if external != my_external:
            if my_external:
                log(f"ATENÇÃO: a porta externa mudou entre consultas ({my_external} -> {external}); "
                    "o NAT não mantém o mapeamento de P (ruim para punching)")
            my_external = external
            _, ext_port = parse_addr(external)
            log(f"endereço externo de P: {external}  ("
                + ("o NAT PRESERVA a porta" if ext_port == port else "o NAT REMAPEIA a porta") + ")")
        for l in lines[1:]:
            parts = l.split()
            if len(parts) == 3 and parts[0] == "PEER":
                peer = parse_addr(parts[2])
                if peer not in punching:
                    punching.add(peer)
                    log(f"par '{parts[1]}' em {parts[2]}: tentando conectar a partir de P")
                    threading.Thread(target=connect_loop, args=(peer,), daemon=True).start()
        if not punching:
            log("esperando o outro lado rodar o script…")
        done.wait(2.0)

    conn = result.get("conn")
    if conn is None:
        done.set()
        log(f"FALHOU depois de {args.timeout:.0f} s. Último erro de conexão: {result.get('last_error', 'nenhum')}")
        log("Possíveis causas: o outro lado não rodou ao mesmo tempo, NAT simétrico, ou o NAT bloqueia TCP de entrada.")
        return 1

    try:
        conn.settimeout(5)
        conn.sendall(f"HELLO {args.label}\n".encode())
        data = conn.recv(256).decode(errors="replace").strip()
        log(f"PUNCH OK: conexão TCP direta {result['how']}; o outro lado disse: {data!r}")
        return 0
    except OSError as exc:
        log(f"a conexão abriu mas a troca de mensagem falhou: {exc}")
        return 1
    finally:
        conn.close()


if __name__ == "__main__":
    sys.exit(main())
