"""Túnel TCP entre Super Peers atrás de NAT (hole punching), sem alterar o código dos Super Peers.

Ideia: o Super Peer só conversa TCP com endereços de loopback. Para cada outro Super Peer
conhecido pelo rendezvous, este módulo:

  1. escuta em 127.x.y.z:<porta do SP remoto>, onde 127.x.y.z é o endereço VIRTUAL desse nó
     (derivado do id dele, igual em todas as máquinas, porque é o ip= que ele divulga);
  2. abre UMA conexão TCP direta com a máquina dele por hole punching (simultaneous open): as
     duas pontas conectam uma na outra, ao mesmo tempo, sempre a partir da MESMA porta local P,
     que também é a porta de onde falam com o rendezvous (assim o NAT usa o mesmo mapeamento);
  3. multiplexa por essa conexão todas as conexões que o Super Peer abrir para o nó remoto.

Do outro lado, cada pedido de abertura vira uma conexão para 127.0.0.1:<porta do próprio SP>.
Só essa porta é aceita (senão a máquina viraria um relay para qualquer serviço local).

Limitações: não cifra nem autentica (o tráfego já era TCP puro); NAT simétrico impede o punching;
a porta do Super Peer remoto não pode ser igual à do local (o SP escuta em 0.0.0.0 e ocuparia o
endereço virtual).
"""

from __future__ import annotations

import asyncio
import hashlib
import os
import random
import socket
import struct
import time
from dataclasses import dataclass, field
from typing import Callable

T_HELLO, T_OPEN, T_DATA, T_CLOSE, T_PING, T_RESET = 1, 2, 3, 4, 5, 6
# T_CLOSE = fim de UMA direção (half-close); T_RESET = aborta o fluxo nas duas
_HDR = struct.Struct("!BII")  # tipo, fluxo, tamanho do payload
MAX_PAYLOAD = 64 * 1024
CHUNK = 16 * 1024
PUNCH_INTERVAL = 3.0       # renova o registro e lê os outros nós
KEEPALIVE = 15.0
DEAD_AFTER = 60.0
LOCAL_WAIT = 20.0          # quanto o SP espera o túnel abrir antes de ter a conexão fechada


def vip(node_id: str) -> str:
    """Endereço virtual de um nó: 127.x.y.z estável derivado do id (`ip:porta`)."""
    h = hashlib.sha256(node_id.encode()).digest()
    return f"127.{1 + h[0] % 253}.{h[1]}.{1 + h[2] % 254}"


def split_id(node_id: str) -> tuple[str, int]:
    ip, _, port = node_id.rpartition(":")
    return ip, int(port)


def split_addr(text: str) -> tuple[str, int]:
    return split_id(text)


def make_socket(port: int = 0) -> socket.socket:
    """Socket TCP não bloqueante que compartilha a porta local (escuta + conexões de saída)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
    s.bind(("0.0.0.0", port))
    s.setblocking(False)
    return s


class Stream:
    """Uma conexão do Super Peer carregada pelo túnel. Cada direção termina separadamente."""

    def __init__(self, writer: asyncio.StreamWriter) -> None:
        self.writer = writer
        self.sent_close = False   # lemos EOF do lado local e avisamos o outro lado
        self.got_close = False    # o outro lado terminou a direção dele


class Link:
    """Conexão TCP perfurada com um nó, multiplexando os fluxos do Super Peer."""

    def __init__(self, tunnel: "Tunnel", node: "Node", reader: asyncio.StreamReader,
                 writer: asyncio.StreamWriter, key: bytes, low: bool) -> None:
        self.tunnel, self.node, self.reader, self.writer, self.key = tunnel, node, reader, writer, key
        self.streams: dict[int, Stream] = {}
        self.closed = asyncio.Event()
        self.last_rx = time.monotonic()
        self._next = 2 if low else 1   # ids pares para o menor id, ímpares para o outro: sem colisão

    @property
    def alive(self) -> bool:
        return not self.closed.is_set()

    async def send(self, typ: int, sid: int, payload: bytes = b"") -> None:
        if self.closed.is_set():
            raise ConnectionError("túnel fechado")
        self.writer.write(_HDR.pack(typ, sid, len(payload)) + payload)
        await self.writer.drain()

    async def open_stream(self, port: int, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        """Liga uma conexão local do Super Peer a um novo fluxo no túnel."""
        sid, self._next = self._next, self._next + 2
        self.streams[sid] = Stream(writer)
        try:
            await self.send(T_OPEN, sid, struct.pack("!H", port))
        except (ConnectionError, OSError):
            await self._drop(sid)
            return
        await self._pump_local(sid, reader)

    async def _pump_local(self, sid: int, reader: asyncio.StreamReader) -> None:
        """Lado local -> túnel. EOF local vira half-close; erro vira reset."""
        try:
            while True:
                data = await reader.read(CHUNK)
                if not data:
                    break
                await self.send(T_DATA, sid, data)
        except (ConnectionError, OSError):
            await self._reset(sid)
            return
        st = self.streams.get(sid)
        if st is None:
            return
        st.sent_close = True
        try:
            await self.send(T_CLOSE, sid)
        except (ConnectionError, OSError):
            await self._drop(sid)
            return
        await self._maybe_finish(sid)

    async def _maybe_finish(self, sid: int) -> None:
        st = self.streams.get(sid)
        if st and st.sent_close and st.got_close:
            await self._drop(sid)

    async def _drop(self, sid: int) -> None:
        """Descarta o fluxo localmente (sem avisar o outro lado)."""
        st = self.streams.pop(sid, None)
        if st:
            try:
                st.writer.close()
            except OSError:
                pass

    async def _reset(self, sid: int) -> None:
        """Aborta o fluxo nas duas pontas."""
        if sid in self.streams:
            await self._drop(sid)
            if self.alive:
                try:
                    await self.send(T_RESET, sid)
                except (ConnectionError, OSError):
                    pass

    async def run(self) -> None:
        """Lê quadros até a conexão cair."""
        tasks = [asyncio.create_task(self._keepalive())]
        try:
            while True:
                hdr = await self.reader.readexactly(_HDR.size)
                typ, sid, ln = _HDR.unpack(hdr)
                if ln > MAX_PAYLOAD:
                    break
                payload = await self.reader.readexactly(ln) if ln else b""
                self.last_rx = time.monotonic()
                if typ == T_OPEN:
                    await self._on_open(sid, payload)
                elif typ == T_DATA:
                    st = self.streams.get(sid)
                    if st is not None:
                        try:
                            st.writer.write(payload)
                            await st.writer.drain()
                        except (ConnectionError, OSError):
                            await self._reset(sid)
                elif typ == T_CLOSE:
                    st = self.streams.get(sid)
                    if st is not None:
                        st.got_close = True
                        try:
                            if st.writer.can_write_eof():
                                st.writer.write_eof()
                        except (ConnectionError, OSError):
                            await self._reset(sid)
                            continue
                        await self._maybe_finish(sid)
                elif typ == T_RESET:
                    await self._drop(sid)
        except (asyncio.IncompleteReadError, ConnectionError, OSError):
            pass
        finally:
            for t in tasks:
                t.cancel()
            await self.close()

    async def _on_open(self, sid: int, payload: bytes) -> None:
        port = struct.unpack("!H", payload[:2])[0] if len(payload) >= 2 else 0
        if port != self.tunnel.allowed_port:
            await self.send(T_RESET, sid)
            return
        try:
            r, w = await asyncio.wait_for(asyncio.open_connection("127.0.0.1", port), 5)
        except (OSError, asyncio.TimeoutError):
            await self.send(T_RESET, sid)
            return
        self.streams[sid] = Stream(w)
        asyncio.create_task(self._pump_local(sid, r))

    async def _keepalive(self) -> None:
        while True:
            await asyncio.sleep(KEEPALIVE)
            if time.monotonic() - self.last_rx > DEAD_AFTER:
                await self.close()
                return
            try:
                await self.send(T_PING, 0)
            except (ConnectionError, OSError):
                return

    async def close(self) -> None:
        if self.closed.is_set():
            return
        self.closed.set()
        for sid in list(self.streams):
            await self._drop(sid)
        try:
            self.writer.close()
        except OSError:
            pass


@dataclass
class Node:
    id: str
    ip: str                       # endereço virtual (127.x.y.z)
    port: int                     # porta do Super Peer dele
    obs: tuple[str, int] | None = None   # endereço externo (onde furar), vindo do rendezvous
    link: Link | None = None
    server: asyncio.AbstractServer | None = None
    task: asyncio.Task | None = None
    link_up: asyncio.Event = field(default_factory=asyncio.Event)
    error: str = ""


class Tunnel:
    def __init__(self, rendezvous: str, punch_port: int, sp_port: int, announce_ip: str = "",
                 on_event: Callable[[str], None] | None = None) -> None:
        self.rendezvous = rendezvous
        self.punch_port = punch_port
        self.sp_port = sp_port
        self.allowed_port = sp_port
        self.announce_ip = announce_ip
        self.me: str | None = None
        self.nodes: dict[str, Node] = {}
        self._event = on_event or (lambda m: None)
        self._listener: socket.socket | None = None
        self._server: asyncio.AbstractServer | None = None
        self._punch_task: asyncio.Task | None = None
        self._rdv: tuple[str, int] | None = None
        self._running = False
        self._last_punch_err = ""

    def event(self, msg: str) -> None:
        self._event(msg)

    # --- ciclo de vida -----------------------------------------------------

    async def start(self) -> str | None:
        """Sobe o túnel. Devolve uma mensagem de erro, ou None se deu certo."""
        host, port = split_addr(self.rendezvous)
        loop = asyncio.get_running_loop()
        try:
            self._rdv = (socket.gethostbyname(host), port)
            self._listener = make_socket(self.punch_port)
            self._listener.listen(16)
        except OSError as exc:
            return f"porta de punching {self.punch_port}: {exc}"
        self._server = await asyncio.start_server(self._on_incoming, sock=self._listener)
        self._running = True
        err = await self._punch_once()
        if err:
            await self.stop()
            return err
        self._punch_task = loop.create_task(self._punch_loop())
        return None

    async def stop(self) -> None:
        self._running = False
        if self._punch_task:
            self._punch_task.cancel()
        for node in self.nodes.values():
            if node.task:
                node.task.cancel()
            if node.server:
                node.server.close()
            if node.link:
                await node.link.close()
        if self._server:
            self._server.close()
        elif self._listener:
            self._listener.close()
        self.nodes.clear()
        self._server = self._listener = None

    # --- rendezvous (a partir da porta P) --------------------------------------

    async def _punch_once(self) -> str | None:
        assert self._rdv
        loop = asyncio.get_running_loop()
        s = make_socket(self.punch_port)
        try:
            await asyncio.wait_for(loop.sock_connect(s, self._rdv), 5)
            reader, writer = await asyncio.open_connection(sock=s)
        except (OSError, asyncio.TimeoutError) as exc:
            s.close()
            return f"rendezvous inacessível: {exc or 'timeout'}"
        try:
            writer.write(f"PUNCH {self.sp_port}{' ' + self.announce_ip if self.announce_ip else ''}\n".encode())
            await writer.drain()
            buf = b""
            while b"END\n" not in buf and not buf.startswith(b"ERR"):
                chunk = await asyncio.wait_for(reader.read(4096), 5)
                if not chunk:
                    break
                buf += chunk
        except (OSError, asyncio.TimeoutError) as exc:
            return f"rendezvous não respondeu: {exc or 'timeout'}"
        finally:
            writer.close()
        lines = buf.decode(errors="replace").splitlines()
        if not lines or not lines[0].startswith("OK "):
            return "resposta inesperada do rendezvous: " + " | ".join(lines)
        parts = lines[0].split()
        if len(parts) != 3:
            return f"o rendezvous não suporta PUNCH (servidor desatualizado?): {lines[0]}"
        if self.me != parts[1]:
            self.me = parts[1]
            self.event(f"meu id: {self.me}; endereço externo da porta {self.punch_port}: {parts[2]}")
        for line in lines[1:]:
            p = line.split()
            if len(p) == 3 and p[0] == "NODE" and p[1] != self.me:
                node = self.ensure_node(p[1])
                if node:
                    node.obs = split_addr(p[2])
        return None

    async def _punch_loop(self) -> None:
        while self._running:
            await asyncio.sleep(PUNCH_INTERVAL)
            try:
                err = await self._punch_once()
            except Exception as exc:  # o laço não pode morrer
                err = str(exc)
            if err and err != self._last_punch_err:
                self.event(err)
            self._last_punch_err = err or ""

    # --- nós ------------------------------------------------------------------------

    def ensure_node(self, node_id: str) -> Node | None:
        """Cria (uma vez) o listener virtual e o laço de conexão de um nó remoto."""
        if node_id == self.me:
            return None
        node = self.nodes.get(node_id)
        if node:
            return node
        ip, port = split_id(node_id)
        node = Node(node_id, vip(node_id), port)
        self.nodes[node_id] = node
        if port == self.sp_port:
            node.error = (f"a porta do Super Peer {node_id} ({port}) é igual à sua; "
                          "troque a sua sp_port, senão não dá para criar o endereço virtual dele")
            self.event(node.error)
            return node
        loop = asyncio.get_running_loop()
        loop.create_task(self._serve_local(node))
        node.task = loop.create_task(self._connector(node))
        return node

    async def _serve_local(self, node: Node) -> None:
        try:
            node.server = await asyncio.start_server(
                lambda r, w: self._on_local(node, r, w), host=node.ip, port=node.port, reuse_address=True)
            self.event(f"nó {node.id}: endereço virtual {node.ip}:{node.port}")
        except OSError as exc:
            node.error = f"não consegui escutar em {node.ip}:{node.port}: {exc}"
            self.event(node.error)

    async def _on_local(self, node: Node, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        """Uma conexão do Super Peer local para um nó remoto: espera o túnel e a encaminha."""
        try:
            await asyncio.wait_for(node.link_up.wait(), LOCAL_WAIT)
        except asyncio.TimeoutError:
            writer.close()
            return
        link = node.link
        if link is None or not link.alive:
            writer.close()
            return
        await link.open_stream(node.port, reader, writer)

    async def wait_links(self, ids: list[str], timeout: float) -> list[str]:
        """Espera até `timeout` s por pelo menos um túnel aberto; devolve os ids com túnel."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            up = [i for i in ids if (n := self.nodes.get(i)) and n.link and n.link.alive]
            if up:
                return up
            await asyncio.sleep(0.3)
        return []

    def status(self) -> tuple[int, int]:
        """(túneis abertos, nós conhecidos)."""
        return sum(1 for n in self.nodes.values() if n.link and n.link.alive), len(self.nodes)

    # --- conexões perfuradas ------------------------------------------------------------

    async def _connector(self, node: Node) -> None:
        loop = asyncio.get_running_loop()
        while self._running:
            if node.link and node.link.alive:
                await node.link.closed.wait()
                await asyncio.sleep(0.5)
                continue
            if node.obs is None:
                await asyncio.sleep(1)
                continue
            s = make_socket(self.punch_port)
            try:
                await asyncio.wait_for(loop.sock_connect(s, node.obs), 2.0)
                reader, writer = await asyncio.open_connection(sock=s)
            except (OSError, asyncio.TimeoutError):
                s.close()
                await asyncio.sleep(0.3 + random.random() * 0.4)
                continue
            await self._setup_link(reader, writer, expected=node)

    async def _on_incoming(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        peer = writer.get_extra_info("peername")
        await self._setup_link(reader, writer, remote_ip=peer[0] if peer else None)

    async def _setup_link(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter,
                          expected: Node | None = None, remote_ip: str | None = None) -> None:
        """Troca HELLO, resolve duplicatas e roda o enlace até ele cair."""
        if not self.me:
            writer.close()
            return
        mine = os.urandom(8)
        try:
            body = mine + self.me.encode()
            writer.write(_HDR.pack(T_HELLO, 0, len(body)) + body)
            await writer.drain()
            typ, _, ln = _HDR.unpack(await asyncio.wait_for(reader.readexactly(_HDR.size), 5))
            if typ != T_HELLO or not 9 <= ln <= 100:
                raise ValueError("HELLO inválido")
            data = await asyncio.wait_for(reader.readexactly(ln), 5)
        except (asyncio.IncompleteReadError, asyncio.TimeoutError, ConnectionError, OSError, ValueError):
            writer.close()
            return
        theirs, peer_id = data[:8], data[8:].decode(errors="replace")
        node = self.nodes.get(peer_id) or (self.ensure_node(peer_id) if ":" in peer_id else None)
        if node is None or peer_id == self.me or (expected and node is not expected):
            writer.close()
            return
        if remote_ip and node.obs and node.obs[0] != remote_ip:
            self.event(f"conexão de {remote_ip} dizendo ser {peer_id} recusada (o rendezvous diz {node.obs[0]})")
            writer.close()
            return
        key = min(mine, theirs) + max(mine, theirs)   # igual nas duas pontas
        link = Link(self, node, reader, writer, key, low=self.me < peer_id)
        old = node.link
        if old and old.alive:
            if key >= old.key:      # fica o enlace de menor chave, nas duas pontas
                writer.close()
                return
            await old.close()
        node.link = link
        node.link_up.set()
        self.event(f"túnel aberto com {node.id}")
        try:
            await link.run()
        finally:
            if node.link is link:
                node.link = None
                node.link_up.clear()
            self.event(f"túnel fechado com {node.id}")
