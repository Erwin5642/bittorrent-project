"""App BitTorrent: um Super Peer e um cliente nesta máquina, com interface de terminal (Textual).

Ao abrir, registra a intenção de criar o Super Peer no servidor de rendezvous, grava o
bootstrap devolvido no sp.conf e só então sobe o Super Peer e o cliente.
Execução: app/run.sh
"""

from __future__ import annotations

import asyncio
import os
import shlex
import signal
import time
from pathlib import Path

from textual import on
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical, VerticalScroll
from textual.screen import ModalScreen
from textual.widgets import (
    Button,
    DataTable,
    DirectoryTree,
    Footer,
    Header,
    Input,
    Label,
    RichLog,
    Static,
    Switch,
    TabbedContent,
    TabPane,
)

import node
import tunnel as tn


def download_target(out: str, name: str) -> str:
    """Destino do download. Vazio, terminado em '/' ou uma pasta existente viram `<pasta>/<nome>`."""
    out = out.strip() or "downloads/"
    resolved = Path(out) if os.path.isabs(out) else node.CLIENT_DIR / out
    if out.endswith("/") or resolved.is_dir():
        out = out.rstrip("/") + "/" + Path(name).name
        resolved = Path(out) if os.path.isabs(out) else node.CLIENT_DIR / out
    resolved.parent.mkdir(parents=True, exist_ok=True)
    return out


class FormModal(ModalScreen["list[str] | None"]):
    DEFAULT_CSS = """
    FormModal { align: center middle; }
    FormModal > Vertical { width: 70; height: auto; border: thick $accent; background: $surface; padding: 1 2; }
    FormModal Horizontal { height: auto; margin-top: 1; align-horizontal: right; }
    FormModal Button { margin-left: 1; }
    """
    BINDINGS = [Binding("escape", "cancel", "Cancelar")]

    def __init__(self, title: str, fields: list[tuple[str, str]]) -> None:
        super().__init__()
        self._title, self._fields = title, fields

    def compose(self) -> ComposeResult:
        with Vertical():
            yield Label(f"[b]{self._title}[/b]")
            for i, (label, default) in enumerate(self._fields):
                yield Label(label)
                yield Input(value=default, id=f"f{i}")
            with Horizontal():
                yield Button("Cancelar", id="cancel")
                yield Button("OK", id="ok", variant="primary")

    def on_mount(self) -> None:
        self.query_one("#f0", Input).focus()

    @on(Button.Pressed, "#ok")
    @on(Input.Submitted)
    def _ok(self) -> None:
        self.dismiss([self.query_one(f"#f{i}", Input).value.strip() for i in range(len(self._fields))])

    @on(Button.Pressed, "#cancel")
    def action_cancel(self) -> None:
        self.dismiss(None)


class UploadModal(ModalScreen["str | None"]):
    """Escolhe um arquivo da máquina (a partir da home) ou digita o caminho."""

    DEFAULT_CSS = """
    UploadModal { align: center middle; }
    UploadModal > Vertical { width: 84; height: 30; border: thick $accent; background: $surface; padding: 1 2; }
    UploadModal DirectoryTree { height: 1fr; }
    UploadModal Horizontal { height: auto; margin-top: 1; align-horizontal: right; }
    UploadModal Button { margin-left: 1; }
    """
    BINDINGS = [Binding("escape", "cancel", "Cancelar")]

    def compose(self) -> ComposeResult:
        with Vertical():
            yield Label("[b]Upload[/b] — escolha um arquivo ou digite o caminho")
            yield DirectoryTree(str(Path.home()))
            yield Input(placeholder="/caminho/do/arquivo", id="path")
            with Horizontal():
                yield Button("Cancelar", id="cancel")
                yield Button("Enviar", id="ok", variant="primary")

    @on(DirectoryTree.FileSelected)
    def _picked(self, event: DirectoryTree.FileSelected) -> None:
        self.query_one("#path", Input).value = str(event.path)

    @on(Button.Pressed, "#ok")
    @on(Input.Submitted)
    def _ok(self) -> None:
        self.dismiss(self.query_one("#path", Input).value.strip() or None)

    @on(Button.Pressed, "#cancel")
    def action_cancel(self) -> None:
        self.dismiss(None)


class BitTorrentApp(App):
    TITLE = "BitTorrent"
    CSS = """
    #status { height: auto; padding: 0 1; background: $boost; }
    .bar { height: auto; margin-bottom: 1; }
    .bar Button { margin-right: 1; }
    .bar Label { margin: 1 1 0 0; }
    #transfers { height: 1fr; min-height: 6; }
    #console, #sp-log { height: 1fr; min-height: 6; border: round $primary; }
    #metrics, #sp-metrics { height: 1fr; min-height: 5; }
    #cfg-log { height: 8; border: round $primary; }
    .cfg Input { width: 60; }
    #client-side { width: 36; border-right: solid $primary; padding: 0 1; }
    """
    BINDINGS = [
        Binding("u", "upload", "Upload"),
        Binding("d", "download", "Download"),
        Binding("l", "lookup", "Buscar"),
        Binding("ctrl+q", "quit", "Sair"),
    ]

    def __init__(self) -> None:
        super().__init__()
        self.cfg = node.Config.load()
        self.sp = node.SuperPeerProc()
        self.client = node.ClientSession()
        self.reg = node.Registration(False, message="ainda não registrado")
        self.tunnel: tn.Tunnel | None = None
        self.ip = ""
        self._seq = 0
        self._stats = {"ok": 0, "erro": 0}
        self._booting = False
        self._quitting = False
        self._log_size = -1

    # --- layout -----------------------------------------------------------

    def compose(self) -> ComposeResult:
        yield Header()
        yield Static("iniciando…", id="status")
        with TabbedContent(id="tabs"):
            with TabPane("Cliente", id="tab-client"):
                with Horizontal():
                    with Vertical(id="client-side"):
                        yield Label("[b]Meu nó[/b] (state)")
                        yield DataTable(id="metrics", show_cursor=False)
                        yield Static("", id="summary")
                        with Horizontal(classes="bar"):
                            yield Switch(id="auto", value=False)
                            yield Label("auto (5 s)")
                            yield Button("Atualizar", id="refresh")
                    with Vertical():
                        with Horizontal(classes="bar"):
                            yield Button("⬆ Upload", id="b-upload", variant="primary")
                            yield Button("⬇ Download", id="b-download", variant="primary")
                            yield Button("🔍 Buscar", id="b-lookup")
                            yield Button("Guardar valor", id="b-put")
                        yield Label("[b]Transferências e operações[/b]")
                        yield DataTable(id="transfers", cursor_type="row")
                        yield Label("[b]Saída[/b]")
                        yield RichLog(id="console", wrap=True, markup=False)
                        yield Input(placeholder="comando livre do cliente (ex.: lookup arquivo.pdf) e Enter", id="cmd")
            with TabPane("Super Peer", id="tab-sp"):
                with Horizontal(classes="bar"):
                    yield Button("Reiniciar nó", id="sp-restart", variant="warning")
                    yield Button("Parar Super Peer", id="sp-stop", variant="error")
                    yield Button("Iniciar Super Peer", id="sp-start", variant="success")
                    yield Button("Re-registrar", id="sp-register")
                    yield Button("Super Peers no rendezvous", id="sp-list")
                yield Static("", id="sp-info")
                yield Label("[b]Estado ao vivo do meu Super Peer[/b]")
                yield DataTable(id="sp-metrics", show_cursor=False)
                with Horizontal(classes="bar"):
                    yield Switch(id="sp-auto", value=False)
                    yield Label("auto (5 s)")
                    yield Button("Atualizar estado", id="sp-refresh")
                    yield Label("filtro do log")
                    yield Input(placeholder="regex (ex.: election|chord)", id="sp-filter")
                yield RichLog(id="sp-log", wrap=False, markup=False, max_lines=3000)
            with TabPane("Configuração", id="tab-cfg"):
                with VerticalScroll(classes="cfg"):
                    yield Label("Servidor de rendezvous (host:porta; vazio = Super Peer isolado)")
                    yield Input(id="cfg-rendezvous", placeholder="192.168.0.10:7000")
                    yield Label("Porta do Super Peer")
                    yield Input(id="cfg-sp_port", restrict=r"\d*")
                    yield Label("Porta do cliente")
                    yield Input(id="cfg-client_port", restrict=r"\d*")
                    yield Label("Porta do túnel (hole punching; também a porta local usada com o rendezvous)")
                    yield Input(id="cfg-punch_port", restrict=r"\d*")
                    yield Label("IP anunciado (opcional; vazio = o IP que o rendezvous enxerga)")
                    yield Input(id="cfg-announce_ip", placeholder="ex.: 200.1.2.3")
                    with Horizontal(classes="bar"):
                        yield Button("Salvar", id="cfg-save", variant="primary")
                        yield Button("Salvar e reiniciar nó", id="cfg-apply", variant="success")
                        yield Button("Testar rendezvous", id="cfg-test")
                    yield RichLog(id="cfg-log", wrap=True, markup=False)
        yield Footer()

    async def on_mount(self) -> None:
        self.sub_title = "Super Peer + cliente desta máquina"
        self.query_one("#transfers", DataTable).add_columns("Hora", "Operação", "Alvo", "Estado")
        self.query_one("#metrics", DataTable).add_columns("Métrica", "Valor")
        self.query_one("#sp-metrics", DataTable).add_columns("Métrica", "Valor")
        self._load_cfg_form()
        self.set_interval(5, self._tick)
        self.set_interval(20, self._renew)
        self.set_interval(1.5, self._follow_log)
        loop = asyncio.get_running_loop()
        for sig in (signal.SIGTERM, signal.SIGHUP):
            loop.add_signal_handler(sig, lambda: asyncio.ensure_future(self.action_quit()))
        self.run_worker(self.boot(), group="boot", exclusive=True)

    # --- partida -------------------------------------------------------------

    def _status(self, text: str) -> None:
        self.query_one("#status", Static).update(text)

    def _refresh_status(self) -> None:
        sp = "[green]rodando[/]" if self.sp.alive else "[red]parado[/]"
        cl = "[green]rodando[/]" if self.client.alive else "[red]parado[/]"
        rdv = (f"[green]registrado como {self.reg.public}[/]" if self.reg.ok
               else f"[yellow]sem rendezvous[/] ({self.reg.message})")
        boot = ", ".join(self.reg.peers) if self.reg.peers else "nenhum (primeiro Super Peer)"
        up, known = self.tunnel.status() if self.tunnel else (0, 0)
        tun = f"{up}/{known} túneis" if self.tunnel else "parado"
        self._status(f"Super Peer: {sp}   Cliente: {cl}   Rendezvous: {rdv}   Túnel: {tun}\nBootstrap: {boot}")
        self.query_one("#sp-info", Static).update(
            f"Endereço virtual anunciado: [b]{self.ip or '—'}:{self.cfg.sp_port}[/b]   "
            f"id no rendezvous: {self.tunnel.me if self.tunnel else '—'}   túneis: {tun}\n"
            f"Config: {node.SP_DIR / 'sp.conf'}   Log: {self.sp.log_path}   Túnel: {node.DATA / 'tunnel.log'}"
        )

    def _blocked(self, reason: str) -> None:
        """Sem rendezvous não há Super Peer nem cliente: mostra o motivo na Configuração."""
        self.query_one("#tabs", TabbedContent).active = "tab-cfg"
        self.query_one("#cfg-log", RichLog).write("✘ " + reason + "\nSuper Peer e cliente não foram iniciados.")
        self._refresh_status()
        self._status(f"[red]{reason}[/]\nSuper Peer e cliente não foram iniciados.")
        self.notify(reason, severity="error", timeout=10)

    async def boot(self) -> None:
        if self._booting:
            return
        self._booting = True
        log = self.query_one("#console", RichLog)
        try:
            self.cfg = node.Config.load()
            errs = self.cfg.errors()
            if errs:
                self.query_one("#tabs", TabbedContent).active = "tab-cfg"
                self.query_one("#cfg-log", RichLog).write("Corrija a configuração:\n" + "\n".join(errs))
                self._status("[red]configuração inválida[/] — nada foi iniciado")
                return
            if not self.cfg.rendezvous:
                self._blocked("Rendezvous não configurado. Informe host:porta na aba Configuração.")
                return
            self._status("registrando no rendezvous…")
            self.reg = await node.register(self.cfg)
            if not self.reg.ok:
                self._blocked(f"Não foi possível usar o rendezvous {self.cfg.rendezvous}: {self.reg.message}")
                return
            # O Super Peer só fala com endereços virtuais de loopback; o túnel leva o tráfego até os
            # outros Super Peers por hole punching TCP. Ele precisa estar de pé antes do Super Peer.
            self._status("abrindo o túnel (hole punching)…")
            self.tunnel = tn.Tunnel(self.cfg.rendezvous, self.cfg.punch_port, self.cfg.sp_port,
                                    self.cfg.announce_ip, on_event=self._tunnel_event)
            err = await self.tunnel.start()
            if err:
                await self._stop_tunnel()
                self._blocked(f"Túnel: {err}")
                return
            self.ip = tn.vip(self.tunnel.me or self.reg.public)
            peers = []
            if self.reg.peers:
                for p in self.reg.peers:
                    self.tunnel.ensure_node(p)
                self._status(f"abrindo túnel com {len(self.reg.peers)} Super Peer(s)… (até 45 s)")
                linked = await self.tunnel.wait_links(self.reg.peers, 45)
                if not linked:
                    await self._stop_tunnel()
                    self._blocked("Nenhum Super Peer respondeu ao hole punching (ele está online? NAT simétrico? "
                                  "veja app/data/tunnel.log). Para não criar uma rede separada, nada foi iniciado")
                    return
                peers = [f"{tn.vip(p)}:{tn.split_id(p)[1]}" for p in linked]
            sp_conf, peer_conf = node.write_confs(self.cfg, self.ip, peers)
            (node.CLIENT_DIR / "downloads").mkdir(parents=True, exist_ok=True)
            self._status("iniciando o Super Peer…")
            err = await self.sp.start(sp_conf)
            if err:
                self.notify(f"Super Peer: {err}", severity="error", timeout=10)
            self._status("iniciando o cliente…")
            err = await self.client.start(peer_conf)
            if err:
                self.notify(f"Cliente: {err}", severity="error", timeout=10)
            self._refresh_status()
            log.write(f"nó iniciado em {self.ip}:{self.cfg.sp_port}")
            self.refresh_metrics()
        finally:
            self._booting = False

    async def _stop_tunnel(self) -> None:
        if self.tunnel:
            await self.tunnel.stop()
            self.tunnel = None

    def _tunnel_event(self, msg: str) -> None:
        """Eventos do túnel vão para app/data/tunnel.log e para o painel de log."""
        line = f"{time.strftime('%H:%M:%S')} {msg}"
        try:
            node.DATA.mkdir(parents=True, exist_ok=True)
            with open(node.DATA / "tunnel.log", "a") as f:
                f.write(line + "\n")
        except OSError:
            pass
        try:
            self.query_one("#sp-log", RichLog).write("[túnel] " + msg)
        except Exception:
            pass

    async def shutdown_node(self) -> None:
        await self.client.stop()
        await self.sp.stop()
        await self._stop_tunnel()
        await node.unregister(self.cfg)

    async def action_quit(self) -> None:
        if self._quitting:
            return
        self._quitting = True
        self._status("encerrando…")
        await self.shutdown_node()
        self.exit()

    async def restart_node(self) -> None:
        self._status("reiniciando…")
        await self.shutdown_node()
        await self.boot()

    # --- comandos do cliente ---------------------------------------------------

    def _need_client(self) -> bool:
        if not self.client.alive or not self.client.ready:
            self.notify("O cliente não está rodando.", severity="warning")
            return False
        return True

    def run_client(self, *cmd: str) -> None:
        if not cmd or not self._need_client():
            return
        self._seq += 1
        key = f"r{self._seq}"
        self.query_one("#transfers", DataTable).add_row(
            time.strftime("%H:%M:%S"), cmd[0], " ".join(cmd[1:]), "⏳ executando", key=key)
        self.run_worker(self._client_job(list(cmd), key), group=f"cmd-{key}")

    async def _client_job(self, cmd: list[str], key: str) -> None:
        table = self.query_one("#transfers", DataTable)
        console = self.query_one("#console", RichLog)
        console.write("$ " + " ".join(shlex.quote(c) for c in cmd))
        rc, lines = await self.client.run(" ".join(shlex.quote(c) for c in cmd), console.write)
        ok = rc == 0
        self._stats["ok" if ok else "erro"] += 1
        self.query_one("#summary", Static).update(
            f"Operações: {sum(self._stats.values())}  [green]ok {self._stats['ok']}[/]  [red]erro {self._stats['erro']}[/]")
        col = list(table.columns.keys())[3]
        table.update_cell(key, col, "[green]✔ concluído[/]" if ok else f"[red]✘ erro (rc={rc})[/]")
        if cmd[0] == "state":
            self._show_metrics("#metrics", lines)

    def action_upload(self) -> None:
        if self._need_client():
            self.push_screen(UploadModal(), lambda p: p and self.run_client("upload", p))

    def action_download(self) -> None:
        if not self._need_client():
            return

        def done(v: list[str] | None) -> None:
            if v and v[0]:
                self.run_client("download", v[0], download_target(v[1], v[0]))

        self.push_screen(FormModal("Download", [("Nome do arquivo na rede", ""),
                                                ("Salvar como (relativo a data/client/)", "downloads/")]), done)

    def action_lookup(self) -> None:
        if self._need_client():
            self.push_screen(FormModal("Buscar", [("Nome ou ObjectID", "")]),
                             lambda v: v and v[0] and self.run_client("lookup", v[0]))

    @on(Button.Pressed, "#b-upload")
    def _b_upload(self) -> None:
        self.action_upload()

    @on(Button.Pressed, "#b-download")
    def _b_download(self) -> None:
        self.action_download()

    @on(Button.Pressed, "#b-lookup")
    def _b_lookup(self) -> None:
        self.action_lookup()

    @on(Button.Pressed, "#b-put")
    def _b_put(self) -> None:
        if self._need_client():
            self.push_screen(FormModal("Guardar valor", [("Nome", ""), ("Valor", "")]),
                             lambda v: v and v[0] and self.run_client("put", v[0], v[1]))

    @on(Input.Submitted, "#cmd")
    def _free_cmd(self, event: Input.Submitted) -> None:
        try:
            parts = shlex.split(event.value)
        except ValueError as exc:
            self.notify(str(exc), severity="error")
            return
        event.input.value = ""
        if parts:
            self.run_client(*parts)

    # --- métricas ----------------------------------------------------------------

    def _show_metrics(self, table_id: str, lines: list[str]) -> None:
        table = self.query_one(table_id, DataTable)
        table.clear()
        for k, v in node.parse_metrics(lines) or [("saída", lines[-1] if lines else "—")]:
            table.add_row(k, v)

    def refresh_metrics(self) -> None:
        if self.client.alive and self.client.ready:
            self.run_worker(self._metrics_job(), group="metrics", exclusive=True)

    async def _metrics_job(self) -> None:
        _, lines = await self.client.run("state", timeout=20)
        self._show_metrics("#metrics", lines)

    @on(Button.Pressed, "#refresh")
    def _refresh_btn(self) -> None:
        self.refresh_metrics()

    def refresh_sp(self) -> None:
        self.run_worker(self._sp_job(), group="sp-metrics", exclusive=True)

    async def _sp_job(self) -> None:
        if not self.sp.alive:
            self._show_metrics("#sp-metrics", ["Super Peer parado"])
            return
        _, lines = await node.sp_state(self.cfg)
        self._show_metrics("#sp-metrics", lines)

    def _tick(self) -> None:
        if self.sp.alive or self.tunnel:
            self._refresh_status()
        if self.query_one("#auto", Switch).value:
            self.refresh_metrics()
        if self.query_one("#sp-auto", Switch).value:
            self.refresh_sp()

    def _follow_log(self) -> None:
        path = self.sp.log_path
        try:
            size = path.stat().st_size
        except OSError:
            return
        if size == self._log_size:
            return
        self._log_size = size
        self._render_log()

    def _render_log(self) -> None:
        log = self.query_one("#sp-log", RichLog)
        log.clear()
        for line in node.tail(self.sp.log_path, 300, self.query_one("#sp-filter", Input).value.strip()):
            log.write(line)

    @on(Input.Changed, "#sp-filter")
    def _filter_changed(self) -> None:
        self._render_log()

    # --- aba Super Peer --------------------------------------------------------------

    @on(Button.Pressed, "#sp-refresh")
    def _sp_refresh(self) -> None:
        self.refresh_sp()

    @on(Button.Pressed, "#sp-restart")
    def _sp_restart(self) -> None:
        self.run_worker(self.restart_node(), group="boot", exclusive=True)

    @on(Button.Pressed, "#sp-stop")
    def _sp_stop(self) -> None:
        async def job() -> None:
            await self.sp.stop()
            await node.unregister(self.cfg)
            self._refresh_status()

        self.run_worker(job(), group="sp-ctl", exclusive=True)

    @on(Button.Pressed, "#sp-start")
    def _sp_start(self) -> None:
        async def job() -> None:
            if not self.cfg.rendezvous:
                self._blocked("Rendezvous não configurado.")
                return
            reg = await node.register(self.cfg)
            if not reg.ok:
                self._blocked(f"Não foi possível usar o rendezvous {self.cfg.rendezvous}: {reg.message}")
                return
            self.reg = reg
            err = await self.sp.start(node.SP_DIR / "sp.conf")
            if err:
                self.notify(f"Super Peer: {err}", severity="error")
            self._refresh_status()

        self.run_worker(job(), group="sp-ctl", exclusive=True)

    @on(Button.Pressed, "#sp-register")
    def _sp_register(self) -> None:
        async def job() -> None:
            self.reg = await node.register(self.cfg)
            self._refresh_status()
            self.notify("Registrado." if self.reg.ok else f"Falhou: {self.reg.message}",
                        severity="information" if self.reg.ok else "error")

        self.run_worker(job(), group="sp-ctl", exclusive=True)

    @on(Button.Pressed, "#sp-list")
    def _sp_list(self) -> None:
        async def job() -> None:
            if not self.cfg.rendezvous:
                self.notify("Rendezvous não configurado.", severity="warning")
                return
            rc, lines = await node.rdv(self.cfg.rendezvous, "list")
            log = self.query_one("#sp-log", RichLog)
            log.write(f"── Super Peers no rendezvous (rc={rc}):")
            for l in lines or ["(nenhum)"]:
                log.write("  " + l)

        self.run_worker(job(), group="sp-ctl", exclusive=True)

    async def _renew(self) -> None:
        if self.sp.alive and self.cfg.rendezvous and not self._booting:
            self.reg = await node.register(self.cfg)
            self._refresh_status()

    # --- configuração -------------------------------------------------------------------

    def _load_cfg_form(self) -> None:
        for key in ("rendezvous", "sp_port", "client_port", "punch_port", "announce_ip"):
            self.query_one(f"#cfg-{key}", Input).value = str(getattr(self.cfg, key))

    def _form_cfg(self) -> node.Config:
        def num(key: str, default: int) -> int:
            v = self.query_one(f"#cfg-{key}", Input).value.strip()
            return int(v) if v.isdigit() else default

        return node.Config(
            rendezvous=self.query_one("#cfg-rendezvous", Input).value.strip(),
            sp_port=num("sp_port", 0),
            client_port=num("client_port", 0),
            punch_port=num("punch_port", 0),
            announce_ip=self.query_one("#cfg-announce_ip", Input).value.strip(),
        )

    def _save_cfg(self) -> bool:
        log = self.query_one("#cfg-log", RichLog)
        cfg = self._form_cfg()
        errs = cfg.errors()
        if errs:
            log.write("✘ não salvo:\n" + "\n".join(f"  - {e}" for e in errs))
            return False
        cfg.save()
        self.cfg = cfg
        log.write(f"✔ salvo em {node.CONFIG_FILE}")
        return True

    @on(Button.Pressed, "#cfg-save")
    def _cfg_save(self) -> None:
        if self._save_cfg():
            self.query_one("#cfg-log", RichLog).write("Vale a partir do próximo início do nó.")

    @on(Button.Pressed, "#cfg-apply")
    def _cfg_apply(self) -> None:
        if self._save_cfg():
            self.run_worker(self.restart_node(), group="boot", exclusive=True)

    @on(Button.Pressed, "#cfg-test")
    def _cfg_test(self) -> None:
        async def job() -> None:
            cfg = self._form_cfg()
            log = self.query_one("#cfg-log", RichLog)
            if not cfg.rendezvous:
                log.write("Informe o rendezvous (host:porta).")
                return
            rc, lines = await node.rdv(cfg.rendezvous, "ping")
            log.write(f"ping {cfg.rendezvous}: " + (" ".join(lines) or "sem resposta") + f" (rc={rc})")

        self.run_worker(job(), group="cfg-test", exclusive=True)


if __name__ == "__main__":
    BitTorrentApp().run()
