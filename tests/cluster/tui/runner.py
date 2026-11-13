"""Executa o cluster.sh e entrega a saída linha a linha (sem cores ANSI)."""

from __future__ import annotations

import asyncio
import os
import re
import signal
from pathlib import Path
from typing import Callable

CLUSTER_DIR = Path(__file__).resolve().parent.parent
CLUSTER_SH = Path(os.environ.get("CLUSTER_SH", CLUSTER_DIR / "cluster.sh"))
CONF_FILE = Path(os.environ.get("CLUSTER_CONF", CLUSTER_DIR / "cluster.conf"))
CONF_EXAMPLE = CLUSTER_DIR / "cluster.conf.example"
FIXTURES_DIR = CLUSTER_DIR / "out" / "files"
REPORT_FILE = CLUSTER_DIR / "out" / "relatorio.md"
SCENARIOS_DIR = CLUSTER_DIR / "scenarios"

_ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
_RC = re.compile(r"^\[rc=(\d+)\]\s*$")


def clean(line: str) -> str:
    return _ANSI.sub("", line.rstrip("\r\n"))


async def stream(args: list[str], on_line: Callable[[str], None], env: dict[str, str] | None = None) -> int:
    """Roda `cluster.sh <args>`; chama on_line por linha. Devolve o código de saída.

    Se a tarefa for cancelada, mata o grupo de processos inteiro (ssh incluso).
    """
    proc = await asyncio.create_subprocess_exec(
        "bash", str(CLUSTER_SH), *args,
        stdin=asyncio.subprocess.DEVNULL,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.STDOUT,
        start_new_session=True,
        cwd=str(CLUSTER_DIR),
        env={**os.environ, "NO_COLOR": "1", **(env or {})},
    )
    try:
        assert proc.stdout is not None
        while True:
            raw = await proc.stdout.readline()
            if not raw:
                break
            on_line(clean(raw.decode(errors="replace")))
        return await proc.wait()
    finally:
        if proc.returncode is None:
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            await proc.wait()


async def capture(args: list[str], env: dict[str, str] | None = None) -> tuple[int, list[str]]:
    lines: list[str] = []
    rc = await stream(args, lines.append, env)
    return rc, lines


def client_rc(lines: list[str]) -> int | None:
    """Código de retorno impresso pelo `client do` (`[rc=N]`)."""
    for line in reversed(lines):
        m = _RC.match(line)
        if m:
            return int(m.group(1))
    return None


def parse_list(lines: list[str]) -> dict[str, list[str]]:
    """Extrai hosts, Super Peers e clientes da saída de `cluster.sh list`."""
    hosts: list[str] = []
    sps: list[str] = []
    clients: list[str] = []
    section = ""
    for line in lines:
        if line.startswith("Hosts"):
            section = "h"
        elif line.startswith("Super Peers"):
            section = "s"
        elif line.startswith("Clientes"):
            section = ""
            m = re.search(r"\b([A-Za-z]+)(\d+)\.\.([A-Za-z]+)(\d+)", line)
            if m:
                clients = [f"{m.group(1)}{i}" for i in range(int(m.group(2)), int(m.group(4)) + 1)]
        elif line.startswith("  ") and line.split():
            (hosts if section == "h" else sps if section == "s" else []).append(line.split()[0])
        elif line and not line.startswith(" "):
            section = ""
    return {"hosts": hosts, "sps": sps, "clients": clients}


def parse_metrics(lines: list[str]) -> list[tuple[str, str]]:
    """Converte linhas `chave: valor` / `chave=valor` em pares para uma tabela."""
    rows: list[tuple[str, str]] = []
    for line in lines:
        if line.startswith("[") or not line.strip():
            continue
        m = re.match(r"^\s*([^:=]{1,40}?)\s*[:=]\s*(.+?)\s*$", line)
        if m:
            rows.append((m.group(1), m.group(2)))
    return rows


def scenarios() -> list[str]:
    return sorted(p.stem for p in SCENARIOS_DIR.glob("*.sh"))


def fixtures() -> list[str]:
    return sorted(p.name for p in FIXTURES_DIR.glob("*") if p.is_file()) if FIXTURES_DIR.exists() else []
