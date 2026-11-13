"""Leitura e escrita do cluster.conf (mesmo formato do lib/conf.sh)."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

DEFAULTS = {
    "sp_per_host": "3",
    "clients_per_host": "30",
    "sp_port_base": "5100",
    "client_port_base": "9000",
    "remote_dir": "~/bittorrent-project",
    "ssh_opts": "-o BatchMode=yes -o StrictHostKeyChecking=accept-new -o ConnectTimeout=5",
}
HOST_KEYS = ("ip", "dir", "sps", "clients", "sp_ports")


@dataclass
class Host:
    alias: str
    target: str
    opts: dict[str, str] = field(default_factory=dict)  # ip, dir, sps, clients, sp_ports
    extra: str = ""  # opções que a TUI não conhece; preservadas como estão


@dataclass
class ClusterConf:
    settings: dict[str, str] = field(default_factory=lambda: dict(DEFAULTS))
    hosts: list[Host] = field(default_factory=list)
    extras: list[str] = field(default_factory=list)  # set desconhecido, node, client (linhas cruas)


def parse(text: str) -> ClusterConf:
    conf = ClusterConf()
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        tok = line.split()
        if tok[0] == "set" and len(tok) >= 3 and tok[1] in DEFAULTS:
            conf.settings[tok[1]] = " ".join(tok[2:])
        elif tok[0] == "host" and len(tok) >= 3:
            host = Host(tok[1], tok[2])
            rest = []
            for opt in tok[3:]:
                key, _, val = opt.partition("=")
                if val and key in HOST_KEYS:
                    host.opts[key] = val
                else:
                    rest.append(opt)
            host.extra = " ".join(rest)
            conf.hosts.append(host)
        else:
            conf.extras.append(line)
    return conf


def dump(conf: ClusterConf) -> str:
    out = [
        "# Topologia do cluster de teste. Gerado pela TUI (tests/cluster/tui); pode ser editado à mão.",
        "# Comentários e a ordem original não são preservados quando a TUI salva.",
        "",
        "# ---- padrões ------------------------------------------------------------",
    ]
    for key in DEFAULTS:
        out.append(f"set {key:<18} {conf.settings.get(key, DEFAULTS[key])}")
    out += ["", "# ---- máquinas -----------------------------------------------------------"]
    for h in conf.hosts:
        parts = ["host", h.alias, h.target]
        parts += [f"{k}={h.opts[k]}" for k in HOST_KEYS if h.opts.get(k)]
        if h.extra:
            parts.append(h.extra)
        out.append(" ".join(parts))
    if conf.extras:
        out += ["", "# ---- avulsos ---------------------------------------------------------------"]
        out += conf.extras
    return "\n".join(out) + "\n"


def load(path: Path) -> ClusterConf:
    return parse(path.read_text()) if path.exists() else ClusterConf()


def check(conf: ClusterConf) -> list[str]:
    """Validações rápidas; a validação completa é a do próprio cluster.sh."""
    errs: list[str] = []
    if not conf.hosts:
        errs.append("Cadastre pelo menos uma máquina.")
    seen: set[str] = set()
    for h in conf.hosts:
        if not h.alias or " " in h.alias:
            errs.append(f"Apelido inválido: {h.alias!r}")
        if h.alias in seen:
            errs.append(f"Apelido repetido: {h.alias}")
        seen.add(h.alias)
    for key in ("sp_per_host", "clients_per_host", "sp_port_base", "client_port_base"):
        if not conf.settings.get(key, "").isdigit():
            errs.append(f"{key} precisa ser um número.")
    return errs
