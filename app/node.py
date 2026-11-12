"""Back-end do app: configuração, registro no rendezvous e processos do Super Peer e do cliente.

Cada máquina roda exatamente um Super Peer e um cliente, ambos filhos deste processo.
"""

from __future__ import annotations

import asyncio
import json
import os
import re
import signal
import socket
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Callable

APP_DIR = Path(__file__).resolve().parent
ROOT = APP_DIR.parent
BIN_DIR = Path(os.environ.get("BT_BIN_DIR", ROOT / "bin"))
RDV_SCRIPT = ROOT / "rendezvous" / "rdv.sh"
DATA = Path(os.environ.get("BT_DATA_DIR", APP_DIR / "data"))
SP_DIR = DATA / "superpeer"
CLIENT_DIR = DATA / "client"
CONFIG_FILE = DATA / "config.json"

_ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
_DONE = re.compile(r"^\[done (\d+) rc=(-?\d+)\]$")
_ADDR = re.compile(r"^(\d{1,3}(?:\.\d{1,3}){3}):(\d{1,5})$")


@dataclass
class Config:
    rendezvous: str = ""        # host:porta; vazio = não registra (Super Peer isolado)
    sp_port: int = 5100         # porta de escuta do Super Peer
    client_port: int = 9000     # identidade do cliente (ele não escuta)
    announce_ip: str = ""       # vazio = o IP que o rendezvous enxerga (ou o IP local sem rendezvous)

    @classmethod
    def load(cls) -> "Config":
        try:
            return cls(**{**asdict(cls()), **json.loads(CONFIG_FILE.read_text())})
        except (OSError, ValueError, TypeError):
            return cls()

    def save(self) -> None:
        DATA.mkdir(parents=True, exist_ok=True)
        CONFIG_FILE.write_text(json.dumps(asdict(self), indent=2) + "\n")

    def errors(self) -> list[str]:
        errs = []
        if self.rendezvous and not re.match(r"^[^\s:]+:\d{1,5}$", self.rendezvous):
            errs.append("Rendezvous deve ser host:porta (ex.: 192.168.0.10:7000).")
        for name, p in (("porta do Super Peer", self.sp_port), ("porta do cliente", self.client_port)):
            if not 1 <= p <= 65535:
                errs.append(f"{name} fora de 1–65535.")
        if self.sp_port == self.client_port:
            errs.append("As portas do Super Peer e do cliente precisam ser diferentes.")
        if self.announce_ip and not re.match(r"^\d{1,3}(\.\d{1,3}){3}$", self.announce_ip):
            errs.append("IP anunciado inválido.")
        return errs


def clean(line: str) -> str:
    return _ANSI.sub("", line.rstrip("\r\n"))


def local_ip() -> str:
    """IP da interface de saída (sem enviar pacote nenhum)."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.0.2.1", 9))
            return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"


# ---------------------------------------------------------------------------
# Rendezvous
# ---------------------------------------------------------------------------


@dataclass
class Registration:
    ok: bool
    public: str = ""            # ip:porta como o servidor nos vê
    peers: list[str] = None     # outros Super Peers (ip:porta)
    message: str = ""

    def __post_init__(self) -> None:
        self.peers = self.peers or []


async def rdv(server: str, *args: str) -> tuple[int, list[str]]:
    proc = await asyncio.create_subprocess_exec(
        "bash", str(RDV_SCRIPT), server, *args,
        stdin=asyncio.subprocess.DEVNULL, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT,
    )
    out, _ = await proc.communicate()
    return proc.returncode or 0, [clean(l) for l in out.decode(errors="replace").splitlines()]


async def register(cfg: Config) -> Registration:
    """Registra a intenção de criar o Super Peer e lê o bootstrap devolvido."""
    if not cfg.rendezvous:
        return Registration(False, message="rendezvous não configurado; subindo como Super Peer isolado")
    args = ["register", str(cfg.sp_port)] + ([cfg.announce_ip] if cfg.announce_ip else [])
    rc, lines = await rdv(cfg.rendezvous, *args)
    if rc != 0:
        return Registration(False, message=" ".join(lines) or f"rdv.sh rc={rc}")
    reg = Registration(True)
    for line in lines:
        kind, _, val = line.partition(" ")
        if kind == "OK" and _ADDR.match(val):
            reg.public = val
        elif kind == "PEER" and _ADDR.match(val):
            reg.peers.append(val)
    if not reg.public:
        return Registration(False, message="resposta inesperada do rendezvous: " + " | ".join(lines))
    return reg


async def unregister(cfg: Config) -> None:
    if cfg.rendezvous:
        await rdv(cfg.rendezvous, "unregister", str(cfg.sp_port), *([cfg.announce_ip] if cfg.announce_ip else []))


# ---------------------------------------------------------------------------
# .conf
# ---------------------------------------------------------------------------


def write_confs(cfg: Config, ip: str, peers: list[str]) -> tuple[Path, Path]:
    """Escreve sp.conf (com bootstrap só se houver outros Super Peers) e peer.conf."""
    SP_DIR.mkdir(parents=True, exist_ok=True)
    CLIENT_DIR.mkdir(parents=True, exist_ok=True)
    sp = ["# Gerado pelo app. Não edite.", f"ip={ip}", f"port={cfg.sp_port}", "type=superpeer"]
    if peers:
        sp.append("bootstrap=" + ",".join(peers))
    sp_conf = SP_DIR / "sp.conf"
    sp_conf.write_text("\n".join(sp) + "\n")
    peer_conf = CLIENT_DIR / "peer.conf"
    peer_conf.write_text(
        "\n".join(["# Gerado pelo app. Não edite.", f"ip={ip}", f"port={cfg.client_port}", "type=peer",
                   f"bootstrap=127.0.0.1:{cfg.sp_port}"]) + "\n"
    )
    return sp_conf, peer_conf


# ---------------------------------------------------------------------------
# Processos
# ---------------------------------------------------------------------------


class SuperPeerProc:
    """O Super Peer desta máquina; saída em data/superpeer/proc.log."""

    def __init__(self) -> None:
        self.proc: asyncio.subprocess.Process | None = None

    @property
    def alive(self) -> bool:
        return self.proc is not None and self.proc.returncode is None

    @property
    def log_path(self) -> Path:
        return SP_DIR / "proc.log"

    async def start(self, conf: Path) -> str | None:
        """Devolve uma mensagem de erro, ou None se subiu."""
        if self.alive:
            return "já está rodando"
        exe = BIN_DIR / "superpeer"
        if not exe.exists():
            return f"{exe} não existe; rode `make` na raiz do projeto"
        SP_DIR.mkdir(parents=True, exist_ok=True)
        log = open(self.log_path, "ab")
        log.write(b"### nova vida\n")
        self.proc = await asyncio.create_subprocess_exec(
            "stdbuf", "-oL", "-eL", str(exe), "--config", str(conf),
            stdin=asyncio.subprocess.DEVNULL, stdout=log, stderr=log, cwd=str(SP_DIR), start_new_session=True,
        )
        log.close()
        await asyncio.sleep(0.5)
        if not self.alive:
            return f"o Super Peer morreu ao subir (ver {self.log_path})"
        return None

    async def stop(self) -> None:
        if self.proc and self.proc.returncode is None:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                await asyncio.wait_for(self.proc.wait(), 5)
            except asyncio.TimeoutError:
                os.killpg(self.proc.pid, signal.SIGKILL)
                await self.proc.wait()


class ClientSession:
    """`bin/client --interactive`: manda um comando e espera o marcador `[done N rc=R]`."""

    def __init__(self) -> None:
        self.proc: asyncio.subprocess.Process | None = None
        self._lock = asyncio.Lock()
        self._lines: asyncio.Queue[str | None] = asyncio.Queue()
        self._reader: asyncio.Task | None = None
        self._seq = 0
        self.ready = False

    @property
    def alive(self) -> bool:
        return self.proc is not None and self.proc.returncode is None

    async def start(self, conf: Path) -> str | None:
        if self.alive:
            return "já está rodando"
        exe = BIN_DIR / "client"
        if not exe.exists():
            return f"{exe} não existe; rode `make` na raiz do projeto"
        CLIENT_DIR.mkdir(parents=True, exist_ok=True)
        self._lines = asyncio.Queue()
        self._seq = 0
        self.ready = False
        self.proc = await asyncio.create_subprocess_exec(
            "stdbuf", "-oL", "-eL", str(exe), "--interactive", "--config", str(conf),
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT,
            cwd=str(CLIENT_DIR), start_new_session=True,
        )
        self._reader = asyncio.create_task(self._read())
        try:
            while True:
                line = await asyncio.wait_for(self._lines.get(), 5)
                if line is None:
                    return "o cliente fechou ao subir"
                if line == "[ready]":
                    self.ready = True
                    return None
        except asyncio.TimeoutError:
            return "o cliente não ficou pronto em 5 s"

    async def _read(self) -> None:
        assert self.proc and self.proc.stdout
        while True:
            raw = await self.proc.stdout.readline()
            if not raw:
                await self._lines.put(None)
                return
            await self._lines.put(clean(raw.decode(errors="replace")))

    async def run(self, command: str, on_line: Callable[[str], None] | None = None, timeout: float = 120) -> tuple[int, list[str]]:
        """Executa um comando do cliente. Devolve (rc, linhas de saída)."""
        async with self._lock:
            if not self.alive or not self.ready or self.proc is None or self.proc.stdin is None:
                return 1, ["cliente não está rodando"]
            self._seq += 1
            seq = self._seq
            self.proc.stdin.write((command + "\n").encode())
            await self.proc.stdin.drain()
            out: list[str] = []
            try:
                while True:
                    line = await asyncio.wait_for(self._lines.get(), timeout)
                    if line is None:
                        return 1, out + ["[o cliente encerrou]"]
                    m = _DONE.match(line)
                    if m and int(m.group(1)) == seq:
                        return int(m.group(2)), out
                    out.append(line)
                    if on_line:
                        on_line(line)
            except asyncio.TimeoutError:
                return 124, out + [f"[sem resposta em {timeout:.0f} s]"]

    async def stop(self) -> None:
        if self.proc and self.proc.returncode is None:
            try:
                if self.proc.stdin:
                    self.proc.stdin.write(b"quit\n")
                    await self.proc.stdin.drain()
                await asyncio.wait_for(self.proc.wait(), 3)
            except (asyncio.TimeoutError, ConnectionError, BrokenPipeError):
                try:
                    os.killpg(self.proc.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                await self.proc.wait()
        if self._reader:
            self._reader.cancel()
        self.ready = False


async def sp_state(cfg: Config) -> tuple[int, list[str]]:
    """Estado ao vivo do Super Peer local (`bin/client --cmd state`)."""
    exe = BIN_DIR / "client"
    if not exe.exists():
        return 1, [f"{exe} não existe"]
    CLIENT_DIR.mkdir(parents=True, exist_ok=True)
    proc = await asyncio.create_subprocess_exec(
        str(exe), "--cmd", "state", "--host", "127.0.0.1", "--port", str(cfg.sp_port),
        stdin=asyncio.subprocess.DEVNULL, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT,
        cwd=str(CLIENT_DIR),
    )
    try:
        out, _ = await asyncio.wait_for(proc.communicate(), 20)
    except asyncio.TimeoutError:
        proc.kill()
        return 124, ["sem resposta em 20 s"]
    return proc.returncode or 0, [clean(l) for l in out.decode(errors="replace").splitlines()]


def tail(path: Path, n: int = 400, pattern: str = "") -> list[str]:
    try:
        lines = path.read_text(errors="replace").splitlines()
    except OSError:
        return []
    if pattern:
        try:
            rx = re.compile(pattern, re.I)
            lines = [l for l in lines if rx.search(l)]
        except re.error:
            lines = [l for l in lines if pattern.lower() in l.lower()]
    return lines[-n:]


def parse_metrics(lines: list[str]) -> list[tuple[str, str]]:
    rows = []
    for line in lines:
        m = re.match(r"^\s*([^:=\[]{1,40}?)\s*[:=]\s*(.+?)\s*$", line)
        if m and not line.startswith("["):
            rows.append((m.group(1), m.group(2)))
    return rows
