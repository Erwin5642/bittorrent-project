"""TUI (Textual) do harness: visão de cliente BitTorrent e visão de gerenciamento do cluster.

Tudo passa pelo tests/cluster/cluster.sh; esta interface não reimplementa nenhuma lógica.
Execução: tests/cluster/tui/run.sh  (ou: python app.py)
"""

from __future__ import annotations

import shlex
import tempfile
import time
from pathlib import Path

from textual import on
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical, VerticalScroll
from textual.screen import ModalScreen, Screen
from textual.widgets import (
    Button,
    DataTable,
    DirectoryTree,
    Footer,
    Header,
    Input,
    Label,
    RichLog,
    Select,
    Static,
    Switch,
    TabbedContent,
    TabPane,
    TextArea,
)

import confio
import runner

CATEGORIES = ["all", "membership", "election", "chord", "2pc", "ist", "lfu", "log", "metadata"]


def blank_to_none(value: object) -> str | None:
    return value if isinstance(value, str) else None


# ---------------------------------------------------------------------------
# Modais
# ---------------------------------------------------------------------------


class FormModal(ModalScreen["list[str] | None"]):
    """Formulário simples: um Input por campo. Devolve a lista de valores ou None."""

    DEFAULT_CSS = """
    FormModal { align: center middle; }
    FormModal > Vertical { width: 70; height: auto; border: thick $accent; background: $surface; padding: 1 2; }
    FormModal Horizontal { height: auto; margin-top: 1; align-horizontal: right; }
    FormModal Button { margin-left: 1; }
    """
    BINDINGS = [Binding("escape", "cancel", "Cancelar")]

    def __init__(self, title: str, fields: list[tuple[str, str]]) -> None:
        super().__init__()
        self._title = title
        self._fields = fields

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

    def _submit(self) -> None:
        self.dismiss([self.query_one(f"#f{i}", Input).value.strip() for i in range(len(self._fields))])

    @on(Button.Pressed, "#ok")
    def _ok(self) -> None:
        self._submit()

    @on(Input.Submitted)
    def _enter(self) -> None:
        self._submit()

    @on(Button.Pressed, "#cancel")
    def action_cancel(self) -> None:
        self.dismiss(None)


class UploadModal(ModalScreen["str | None"]):
    """Escolhe um arquivo de teste (out/files) ou digita o caminho relativo a run/<cliente>/."""

    DEFAULT_CSS = """
    UploadModal { align: center middle; }
    UploadModal > Vertical { width: 80; height: 28; border: thick $accent; background: $surface; padding: 1 2; }
    UploadModal DirectoryTree { height: 1fr; }
    UploadModal Horizontal { height: auto; margin-top: 1; align-horizontal: right; }
    UploadModal Button { margin-left: 1; }
    """
    BINDINGS = [Binding("escape", "cancel", "Cancelar")]

    def compose(self) -> ComposeResult:
        with Vertical():
            yield Label("[b]Upload[/b] — escolha um arquivo ou digite o caminho (relativo a run/<cliente>/)")
            if runner.FIXTURES_DIR.exists():
                yield DirectoryTree(str(runner.FIXTURES_DIR))
            yield Input(value="../files/", id="path")
            with Horizontal():
                yield Button("Cancelar", id="cancel")
                yield Button("Enviar", id="ok", variant="primary")

    @on(DirectoryTree.FileSelected)
    def _picked(self, event: DirectoryTree.FileSelected) -> None:
        self.query_one("#path", Input).value = f"../files/{event.path.name}"

    @on(Button.Pressed, "#ok")
    @on(Input.Submitted)
    def _ok(self) -> None:
        self.dismiss(self.query_one("#path", Input).value.strip() or None)

    @on(Button.Pressed, "#cancel")
    def action_cancel(self) -> None:
        self.dismiss(None)


# ---------------------------------------------------------------------------
# Visão do cliente (aplicação BitTorrent tradicional)
# ---------------------------------------------------------------------------


class ClientScreen(Screen):
    CSS = """
    #side { width: 38; border-right: solid $primary; padding: 0 1; }
    #main { padding: 0 1; }
    #toolbar { height: auto; }
    #toolbar Button { margin-right: 1; }
    #transfers { height: 1fr; min-height: 6; }
    #console { height: 1fr; min-height: 6; border: round $primary; }
    #metrics { height: 1fr; }
    #summary { margin: 1 0; }
    .row { height: auto; }
    .row Label { margin: 1 1 0 0; }
    """
    BINDINGS = [
        Binding("u", "upload", "Upload"),
        Binding("d", "download", "Download"),
        Binding("l", "lookup", "Buscar"),
        Binding("r", "refresh_metrics", "Atualizar métricas"),
    ]

    def __init__(self) -> None:
        super().__init__()
        self.client: str | None = None
        self._seq = 0
        self._stats = {"ok": 0, "erro": 0}
        self._metrics_busy = False

    def compose(self) -> ComposeResult:
        yield Header()
        with Horizontal():
            with Vertical(id="side"):
                yield Label("[b]Cliente[/b]")
                yield Select([], id="client", prompt="escolha…", allow_blank=True)
                with Horizontal(classes="row"):
                    yield Button("Iniciar", id="start", variant="success")
                    yield Button("Parar", id="stop", variant="warning")
                yield Static("", id="status")
                yield Static("", id="summary")
                yield Label("[b]Métricas do nó[/b] (state)")
                yield DataTable(id="metrics", show_cursor=False)
                with Horizontal(classes="row"):
                    yield Switch(value=False, id="auto")
                    yield Label("auto (5 s)")
                    yield Button("Atualizar", id="refresh")
            with Vertical(id="main"):
                with Horizontal(id="toolbar"):
                    yield Button("⬆ Upload", id="b-upload", variant="primary")
                    yield Button("⬇ Download", id="b-download", variant="primary")
                    yield Button("🔍 Buscar", id="b-lookup")
                    yield Button("Guardar valor", id="b-put")
                    yield Button("Ping", id="b-ping")
                    yield Button("Join", id="b-join")
                    yield Button("Leave", id="b-leave")
                yield Label("[b]Transferências e operações[/b]")
                yield DataTable(id="transfers", cursor_type="row")
                yield Label("[b]Saída[/b]")
                yield RichLog(id="console", wrap=True, markup=False, highlight=False)
                yield Input(placeholder="comando livre do cliente (ex.: lookup documento_a.pdf) e Enter", id="cmd")
        yield Footer()

    async def on_mount(self) -> None:
        self.title = "BitTorrent — Cliente"
        self.query_one("#transfers", DataTable).add_columns("Hora", "Operação", "Alvo", "Estado")
        self.query_one("#metrics", DataTable).add_columns("Métrica", "Valor")
        self.set_interval(5, self._auto_tick)
        await self.reload_clients()

    async def reload_clients(self) -> None:
        rc, lines = await runner.capture(["list"])
        clients = runner.parse_list(lines)["clients"]
        sel = self.query_one("#client", Select)
        sel.set_options([(c, c) for c in clients])
        if clients:
            sel.value = clients[0]
        elif rc != 0:
            self._console("\n".join(lines))

    # --- helpers ----------------------------------------------------------

    def _console(self, text: str) -> None:
        log = self.query_one("#console", RichLog)
        for line in text.splitlines():
            log.write(line)

    def _need_client(self) -> str | None:
        if not self.client:
            self.notify("Escolha um cliente primeiro.", severity="warning")
        return self.client

    def _update_summary(self) -> None:
        self.query_one("#summary", Static).update(
            f"Operações: {self._stats['ok'] + self._stats['erro']}  "
            f"[green]ok {self._stats['ok']}[/]  [red]erro {self._stats['erro']}[/]"
        )

    @on(Select.Changed, "#client")
    def _client_changed(self, event: Select.Changed) -> None:
        self.client = blank_to_none(event.value)
        self.query_one("#status", Static).update(f"Ativo: [b]{self.client or '—'}[/b]")
        self.query_one("#metrics", DataTable).clear()
        if self.client:
            self.refresh_metrics()

    # --- execução de comandos do cliente -----------------------------------

    def run_client(self, *cmd: str) -> None:
        client = self._need_client()
        if not client or not cmd:
            return
        self._seq += 1
        key = f"r{self._seq}"
        table = self.query_one("#transfers", DataTable)
        target = " ".join(cmd[1:])
        table.add_row(time.strftime("%H:%M:%S"), cmd[0], target, "⏳ executando", key=key)
        self.run_worker(self._client_job(client, list(cmd), key), group=f"cmd-{key}")

    async def _client_job(self, client: str, cmd: list[str], key: str) -> None:
        table = self.query_one("#transfers", DataTable)
        console = self.query_one("#console", RichLog)
        console.write(f"$ {client}> {' '.join(shlex.quote(c) for c in cmd)}")
        lines: list[str] = []
        rc = await runner.stream(["client", "do", client, *cmd], lambda l: (lines.append(l), console.write(l)))
        crc = runner.client_rc(lines)
        ok = rc == 0 and (crc in (None, 0))
        self._stats["ok" if ok else "erro"] += 1
        self._update_summary()
        col = list(table.columns.keys())[3]
        table.update_cell(key, col, "[green]✔ concluído[/]" if ok else f"[red]✘ erro (rc={crc if crc is not None else rc})[/]")
        if cmd[0] == "state":
            self._show_metrics(lines)

    # --- ações -------------------------------------------------------------

    def action_upload(self) -> None:
        if not self._need_client():
            return
        self.app.push_screen(UploadModal(), lambda p: p and self.run_client("upload", p))

    def action_download(self) -> None:
        if not self._need_client():
            return

        def done(v: list[str] | None) -> None:
            if v and v[0]:
                self.run_client("download", v[0], v[1] or v[0])

        self.app.push_screen(FormModal("Download", [("Nome do arquivo na rede", ""), ("Salvar como", "baixado.bin")]), done)

    def action_lookup(self) -> None:
        if not self._need_client():
            return
        self.app.push_screen(
            FormModal("Buscar", [("Nome ou ObjectID", "")]), lambda v: v and v[0] and self.run_client("lookup", v[0])
        )

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
        if not self._need_client():
            return
        self.app.push_screen(
            FormModal("Guardar valor", [("Nome", ""), ("Valor", "")]),
            lambda v: v and v[0] and self.run_client("put", v[0], v[1]),
        )

    @on(Button.Pressed, "#b-ping")
    @on(Button.Pressed, "#b-join")
    @on(Button.Pressed, "#b-leave")
    def _b_control(self, event: Button.Pressed) -> None:
        assert event.button.id
        self.run_client(event.button.id.removeprefix("b-"))

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

    @on(Button.Pressed, "#start")
    def _start(self) -> None:
        c = self._need_client()
        if c:
            self.run_worker(self._plain(["client", "start", c]), group="proc")

    @on(Button.Pressed, "#stop")
    def _stop(self) -> None:
        c = self._need_client()
        if c:
            self.run_worker(self._plain(["client", "stop", c]), group="proc")

    async def _plain(self, args: list[str]) -> None:
        console = self.query_one("#console", RichLog)
        console.write("$ cluster.sh " + " ".join(args))
        await runner.stream(args, console.write)

    # --- métricas -----------------------------------------------------------

    @on(Button.Pressed, "#refresh")
    def action_refresh_metrics(self) -> None:
        self.refresh_metrics()

    def refresh_metrics(self) -> None:
        if self.client and not self._metrics_busy:
            self.run_worker(self._metrics_job(self.client), group="metrics", exclusive=True)

    async def _metrics_job(self, client: str) -> None:
        self._metrics_busy = True
        try:
            _, lines = await runner.capture(["client", "do", client, "state"])
            self._show_metrics(lines)
        finally:
            self._metrics_busy = False

    def _show_metrics(self, lines: list[str]) -> None:
        table = self.query_one("#metrics", DataTable)
        table.clear()
        rows = runner.parse_metrics(lines) or [("saída", lines[-1] if lines else "—")]
        for k, v in rows:
            table.add_row(k, v)

    def _auto_tick(self) -> None:
        if self.query_one("#auto", Switch).value:
            self.refresh_metrics()


# ---------------------------------------------------------------------------
# Visão de gerenciamento do cluster
# ---------------------------------------------------------------------------

# id do botão -> (id do log, argumentos do cluster.sh)
ACTIONS: dict[str, tuple[str, list[str]]] = {
    "a-check": ("log-cluster", ["check"]),
    "a-deploy": ("log-cluster", ["deploy"]),
    "a-up": ("log-cluster", ["up"]),
    "a-upc": ("log-cluster", ["up", "--clients"]),
    "a-down": ("log-cluster", ["down"]),
    "a-status": ("log-cluster", ["status", "--clients"]),
    "a-net": ("log-cluster", ["net"]),
    "a-clean": ("log-cluster", ["clean"]),
    "m-overview": ("log-metrics", ["overview"]),
    "m-members": ("log-metrics", ["members"]),
    "m-chord": ("log-metrics", ["chord", "--fingers"]),
    "m-coordinator": ("log-metrics", ["coordinator"]),
    "m-state": ("log-metrics", ["state", "--files"]),
    "m-metadata": ("log-metrics", ["metadata"]),
}


class ManagerScreen(Screen):
    CSS = """
    .bar { height: auto; margin-bottom: 1; }
    .bar Button { margin-right: 1; }
    .bar Select { width: 24; margin-right: 1; }
    .bar Input { width: 40; margin-right: 1; }
    .bar Label { margin: 1 1 0 0; }
    RichLog { height: 1fr; border: round $primary; }
    #log-conf { height: 8; }
    #cf-hosts { height: 9; }
    #cf-extras { height: 6; }
    .cf-num { width: 10; margin-right: 2; }
    .cf-wide { width: 90; }
    """

    def __init__(self) -> None:
        super().__init__()
        self.conf = confio.ClusterConf()
        self._saved_dump = ""
        self._last_metric: str | None = "m-overview"
        self.topology: dict[str, list[str]] = {"hosts": [], "sps": [], "clients": []}

    def compose(self) -> ComposeResult:
        yield Header()
        with TabbedContent():
            with TabPane("Configuração", id="t-conf"):
                with VerticalScroll():
                    yield Label("[b]Padrões[/b]  (gravados em cluster.conf; vale para o próximo [i]Subir[/i])")
                    with Horizontal(classes="bar"):
                        for key, label in (
                            ("sp_per_host", "SPs/máquina"),
                            ("clients_per_host", "clientes/máquina"),
                            ("sp_port_base", "porta base SP"),
                            ("client_port_base", "porta base cliente"),
                        ):
                            yield Label(label)
                            yield Input(id=f"cf-{key}", restrict=r"\d*", classes="cf-num")
                    with Horizontal(classes="bar"):
                        yield Label("remote_dir")
                        yield Input(id="cf-remote_dir", classes="cf-wide")
                    with Horizontal(classes="bar"):
                        yield Label("ssh_opts")
                        yield Input(id="cf-ssh_opts", classes="cf-wide")
                    yield Label("[b]Máquinas[/b]")
                    yield DataTable(id="cf-hosts", cursor_type="row")
                    with Horizontal(classes="bar"):
                        yield Button("Adicionar", id="cf-add")
                        yield Button("Editar", id="cf-edit")
                        yield Button("Remover", id="cf-del", variant="error")
                    yield Label("[b]Avulsos[/b]  (linhas node/client e demais diretivas, uma por linha)")
                    yield TextArea(id="cf-extras")
                    with Horizontal(classes="bar"):
                        yield Button("Salvar", id="cf-save", variant="success")
                        yield Button("Recarregar do arquivo", id="cf-reload")
                        yield Button("Carregar exemplo", id="cf-example")
                    yield RichLog(id="log-conf", wrap=True, markup=False)
            with TabPane("Cluster", id="t-cluster"):
                with Horizontal(classes="bar"):
                    yield Button("Subir", id="a-up", variant="success")
                    yield Button("Subir + clientes", id="a-upc", variant="success")
                    yield Button("Derrubar", id="a-down", variant="error")
                    yield Button("Status", id="a-status")
                    yield Button("Rede (net)", id="a-net")
                with Horizontal(classes="bar"):
                    yield Button("Check", id="a-check")
                    yield Button("Deploy (sync+build)", id="a-deploy")
                    yield Button("Limpar logs", id="a-clean")
                with Horizontal(classes="bar"):
                    yield Select([], id="proc", prompt="processo…")
                    yield Select([(o, o) for o in ("kill", "stop", "start", "restart")], id="op", value="restart", allow_blank=False)
                    yield Button("Aplicar", id="a-proc")
                yield RichLog(id="log-cluster", wrap=True, markup=False)
            with TabPane("Métricas", id="t-metrics"):
                with Horizontal(classes="bar"):
                    yield Button("Overview", id="m-overview", variant="primary")
                    yield Button("Membership", id="m-members")
                    yield Button("Chord", id="m-chord")
                    yield Button("Coordenador", id="m-coordinator")
                    yield Button("Estado/LFU", id="m-state")
                    yield Button("Metadata", id="m-metadata")
                with Horizontal(classes="bar"):
                    yield Input(placeholder="lookup: nome ou ObjectID", id="lookup-q")
                    yield Button("Lookup", id="m-lookup")
                    yield Switch(value=False, id="auto-m")
                    yield Label("auto (5 s)")
                with Horizontal(classes="bar"):
                    yield Select([], id="ev-sp", prompt="SP…")
                    yield Select([(c, c) for c in CATEGORIES], id="ev-cat", value="all", allow_blank=False)
                    yield Input(value="50", id="ev-n", restrict=r"\d*")
                    yield Button("Eventos", id="m-events")
                yield RichLog(id="log-metrics", wrap=True, markup=False)
            with TabPane("Testes", id="t-tests"):
                with Horizontal(classes="bar"):
                    yield Select([(s, s) for s in runner.scenarios()], id="scenario", prompt="cenário…")
                    yield Button("Executar", id="t-run", variant="success")
                    yield Button("Parar", id="t-stop", variant="error")
                    yield Button("Mostrar relatório", id="t-report")
                yield RichLog(id="log-tests", wrap=True, markup=False)
            with TabPane("Comandos", id="t-exec"):
                with Horizontal(classes="bar"):
                    yield Select([], id="host", prompt="máquina…")
                    yield Input(placeholder="comando shell a rodar na máquina (via ssh)", id="exec-cmd")
                    yield Button("Executar", id="x-run", variant="primary")
                yield RichLog(id="log-exec", wrap=True, markup=False)
            with TabPane("Logs", id="t-logs"):
                with Horizontal(classes="bar"):
                    yield Select([], id="log-proc", prompt="processo…")
                    yield Button("Seguir", id="l-follow", variant="primary")
                    yield Button("Parar", id="l-stop")
                yield RichLog(id="log-follow", wrap=False, markup=False, max_lines=5000)
        yield Footer()

    async def on_mount(self) -> None:
        self.title = "BitTorrent — Gerenciamento do cluster"
        self.set_interval(5, self._auto_tick)
        self.query_one("#cf-hosts", DataTable).add_columns("Apelido", "Alvo ssh", "ip", "dir", "sps", "clients", "sp_ports")
        self.load_form(confio.load(runner.CONF_FILE))
        self._saved_dump = confio.dump(self.conf)
        await self._load_topology()

    async def _load_topology(self) -> None:
        rc, lines = await runner.capture(["list"])
        self.topology = runner.parse_list(lines)
        sps, hosts, clients = self.topology["sps"], self.topology["hosts"], self.topology["clients"]
        procs = [(p, p) for p in sps + clients]
        for sel, opts in (("#proc", procs), ("#log-proc", procs), ("#ev-sp", [(s, s) for s in sps]), ("#host", [(h, h) for h in hosts])):
            self.query_one(sel, Select).set_options(opts)
        if rc != 0:
            self.query_one("#log-cluster", RichLog).write("\n".join(lines))

    # --- configuração (cluster.conf) --------------------------------------------

    def load_form(self, conf: confio.ClusterConf) -> None:
        self.conf = conf
        for key in confio.DEFAULTS:
            self.query_one(f"#cf-{key}", Input).value = conf.settings.get(key, confio.DEFAULTS[key])
        self.query_one("#cf-extras", TextArea).text = "\n".join(conf.extras)
        self._refresh_hosts()

    def _refresh_hosts(self) -> None:
        table = self.query_one("#cf-hosts", DataTable)
        table.clear()
        for h in self.conf.hosts:
            table.add_row(h.alias, h.target, *(h.opts.get(k, "") for k in confio.HOST_KEYS))

    def form_conf(self) -> confio.ClusterConf:
        conf = confio.ClusterConf(hosts=self.conf.hosts)
        for key in confio.DEFAULTS:
            conf.settings[key] = self.query_one(f"#cf-{key}", Input).value.strip()
        conf.extras = [l.strip() for l in self.query_one("#cf-extras", TextArea).text.splitlines() if l.strip()]
        return conf

    def _host_form(self, host: confio.Host | None, done) -> None:
        h = host or confio.Host("", "")
        fields = [("Apelido", h.alias), ("Alvo (usuario@endereco ou local)", h.target)]
        fields += [(k + " (opcional)", h.opts.get(k, "")) for k in confio.HOST_KEYS]
        self.app.push_screen(FormModal("Máquina", fields), done)

    def _host_from_values(self, v: list[str], old: confio.Host | None) -> confio.Host:
        return confio.Host(v[0], v[1], {k: val for k, val in zip(confio.HOST_KEYS, v[2:]) if val}, old.extra if old else "")

    def _cf_add(self) -> None:
        def done(v: list[str] | None) -> None:
            if v and v[0] and v[1]:
                self.conf.hosts.append(self._host_from_values(v, None))
                self._refresh_hosts()

        self._host_form(None, done)

    def _cf_edit(self) -> None:
        table = self.query_one("#cf-hosts", DataTable)
        idx = table.cursor_row
        if not self.conf.hosts or not 0 <= idx < len(self.conf.hosts):
            self.notify("Selecione uma máquina na tabela.", severity="warning")
            return
        old = self.conf.hosts[idx]

        def done(v: list[str] | None) -> None:
            if v and v[0] and v[1]:
                self.conf.hosts[idx] = self._host_from_values(v, old)
                self._refresh_hosts()

        self._host_form(old, done)

    def _cf_del(self) -> None:
        idx = self.query_one("#cf-hosts", DataTable).cursor_row
        if self.conf.hosts and 0 <= idx < len(self.conf.hosts):
            del self.conf.hosts[idx]
            self._refresh_hosts()

    async def _cf_save(self) -> None:
        log = self.query_one("#log-conf", RichLog)
        conf = self.form_conf()
        errs = confio.check(conf)
        if errs:
            log.write("✘ não salvo:\n" + "\n".join(f"  - {e}" for e in errs))
            return
        text = confio.dump(conf)
        with tempfile.TemporaryDirectory() as tmp:
            probe = Path(tmp) / "cluster.conf"
            probe.write_text(text)
            rc, lines = await runner.capture(["list"], env={"CLUSTER_CONF": str(probe)})
        if rc != 0:
            log.write("✘ o cluster.sh rejeitou a configuração (nada foi gravado):\n" + "\n".join(lines))
            return
        if runner.CONF_FILE.exists():
            runner.CONF_FILE.with_suffix(".conf.bak").write_text(runner.CONF_FILE.read_text())
        runner.CONF_FILE.write_text(text)
        self.conf = conf
        self._saved_dump = text
        log.write(f"✔ gravado em {runner.CONF_FILE} (backup em .conf.bak)\n" + "\n".join(lines))
        log.write("Se o cluster já estiver de pé, derrube e suba de novo para aplicar.")
        await self._load_topology()
        self.run_worker(self.app.get_screen("client").reload_clients(), group="reload-clients")

    def _unsaved(self) -> bool:
        return confio.dump(self.form_conf()) != self._saved_dump

    # --- execução -------------------------------------------------------------

    def run_into(self, log_id: str, args: list[str], group: str | None = None) -> None:
        self.run_worker(self._job(log_id, args), group=group or log_id, exclusive=True)

    async def _job(self, log_id: str, args: list[str]) -> None:
        log = self.query_one(f"#{log_id}", RichLog)
        log.write(f"── $ cluster.sh {' '.join(shlex.quote(a) for a in args)}  [{time.strftime('%H:%M:%S')}]")
        rc = await runner.stream(args, log.write)
        log.write(f"── fim (rc={rc})")

    def _sel(self, sel_id: str) -> str | None:
        return blank_to_none(self.query_one(f"#{sel_id}", Select).value)

    @on(Button.Pressed)
    def _pressed(self, event: Button.Pressed) -> None:
        bid = event.button.id or ""
        if bid in ("a-up", "a-upc", "a-deploy") and self._unsaved():
            self.notify("Há alterações não salvas na aba Configuração. Salve antes de subir.", severity="warning")
            return
        if bid == "cf-add":
            self._cf_add()
        elif bid == "cf-edit":
            self._cf_edit()
        elif bid == "cf-del":
            self._cf_del()
        elif bid == "cf-save":
            self.run_worker(self._cf_save(), group="cf-save", exclusive=True)
        elif bid == "cf-reload":
            self.load_form(confio.load(runner.CONF_FILE))
            self.query_one("#log-conf", RichLog).write(f"recarregado de {runner.CONF_FILE}")
        elif bid == "cf-example":
            self.load_form(confio.load(runner.CONF_EXAMPLE))
            self.query_one("#log-conf", RichLog).write("exemplo carregado no formulário (use Salvar para gravar)")
        elif bid in ACTIONS:
            log_id, args = ACTIONS[bid]
            if bid.startswith("m-"):
                self._last_metric = bid
            self.run_into(log_id, args)
        elif bid == "a-proc":
            proc, op = self._sel("proc"), self._sel("op")
            if proc and op:
                self.run_into("log-cluster", [op, proc])
            else:
                self.notify("Escolha o processo e a operação.", severity="warning")
        elif bid == "m-lookup":
            q = self.query_one("#lookup-q", Input).value.strip()
            if q:
                self._last_metric = None
                self.run_into("log-metrics", ["lookup", q])
        elif bid == "m-events":
            sp = self._sel("ev-sp")
            if sp:
                self._last_metric = None
                n = self.query_one("#ev-n", Input).value or "50"
                self.run_into("log-metrics", ["events", sp, self._sel("ev-cat") or "all", n])
            else:
                self.notify("Escolha um SP.", severity="warning")
        elif bid == "t-run":
            sc = self._sel("scenario")
            if sc:
                self.query_one("#log-tests", RichLog).clear()
                self.run_into("log-tests", ["run", sc], group="tests")
            else:
                self.notify("Escolha um cenário.", severity="warning")
        elif bid == "t-stop":
            self.workers.cancel_group(self, "tests")
            self.query_one("#log-tests", RichLog).write("── interrompido pelo usuário (o run.sh derruba o cluster ao sair)")
        elif bid == "t-report":
            log = self.query_one("#log-tests", RichLog)
            if runner.REPORT_FILE.exists():
                log.write(runner.REPORT_FILE.read_text())
            else:
                log.write("Sem relatório em out/relatorio.md ainda.")
        elif bid == "x-run":
            host, cmd = self._sel("host"), self.query_one("#exec-cmd", Input).value.strip()
            if host and cmd:
                self.run_into("log-exec", ["exec", host, cmd])
            else:
                self.notify("Escolha a máquina e digite o comando.", severity="warning")
        elif bid == "l-follow":
            p = self._sel("log-proc")
            if p:
                self.query_one("#log-follow", RichLog).clear()
                self.run_worker(self._follow(p), group="follow", exclusive=True)
        elif bid == "l-stop":
            self.workers.cancel_group(self, "follow")

    @on(Input.Submitted, "#exec-cmd")
    def _exec_enter(self) -> None:
        host, cmd = self._sel("host"), self.query_one("#exec-cmd", Input).value.strip()
        if host and cmd:
            self.run_into("log-exec", ["exec", host, cmd])

    async def _follow(self, name: str) -> None:
        log = self.query_one("#log-follow", RichLog)
        log.write(f"── seguindo {name} (Parar encerra)")
        await runner.stream(["logs", name], log.write)

    def _auto_tick(self) -> None:
        if self._last_metric and self.query_one("#auto-m", Switch).value:
            log_id, args = ACTIONS[self._last_metric]
            self.query_one(f"#{log_id}", RichLog).clear()
            self.run_into(log_id, args)


# ---------------------------------------------------------------------------


class ClusterApp(App):
    TITLE = "BitTorrent"
    SCREENS = {"client": ClientScreen, "manager": ManagerScreen}
    BINDINGS = [
        Binding("f1", "show('client')", "Cliente"),
        Binding("f2", "show('manager')", "Gerenciamento"),
        Binding("ctrl+q", "quit", "Sair"),
    ]

    def on_mount(self) -> None:
        self.push_screen("client")

    def action_show(self, name: str) -> None:
        current = self.screen
        if current is not self.get_screen(name):
            self.switch_screen(name)


if __name__ == "__main__":
    ClusterApp().run()
