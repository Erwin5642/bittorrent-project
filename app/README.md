# App BitTorrent (cliente final)

Interface de terminal (Textual) que roda **um Super Peer e um cliente** nesta máquina.

```bash
make                 # na raiz: gera bin/superpeer e bin/client
app/run.sh           # cria app/.venv na primeira vez (precisa de rede)
```

## O que acontece ao abrir

1. Lê `app/data/config.json` (rendezvous, portas, IP anunciado).
2. Roda `rendezvous/rdv.sh <servidor> register <porta-do-SP>`: o servidor guarda o endereço
   público e devolve até 3 outros Super Peers.
3. Sobe o **túnel** (hole punching TCP, `app/tunnel.py`) e espera abrir a conexão com um dos Super Peers
   devolvidos (até 45 s). Sem nenhum túnel, nada é iniciado (para não criar uma rede separada).
4. Grava `app/data/superpeer/sp.conf`: `ip=` é o IP público devolvido; `bootstrap=` só aparece
   se o servidor devolveu algum Super Peer.
   **Sem rendezvous configurado, ou se ele não responder, nada é iniciado**: o app abre a aba
   Configuração com o motivo.
5. Sobe o Super Peer e o cliente (`bin/client --interactive`, falando com o Super Peer local).
6. Renova o registro a cada 20 s. Ao sair (`Ctrl+Q`), faz `unregister` e encerra os processos.

## Como o túnel funciona (sem alterar o código dos Super Peers)

O Super Peer só fala TCP com endereços de loopback. Cada nó tem um **endereço virtual** `127.x.y.z`
derivado do id dele (`ip:porta` no rendezvous), igual em todas as máquinas; é esse o `ip=` do
`sp.conf` e o que os Super Peers divulgam entre si. Para cada outro Super Peer, o app escuta em
`127.x.y.z:<porta dele>` e leva as conexões por **uma** conexão TCP direta, aberta por hole punching:
as duas pontas conectam uma na outra ao mesmo tempo, sempre a partir da mesma porta local (a "porta do
túnel", também usada para falar com o rendezvous, comando `PUNCH`). Do outro lado, o app entrega em
`127.0.0.1:<porta do próprio Super Peer>`; qualquer outra porta é recusada. Eventos em `app/data/tunnel.log`.

Limitações: não cifra nem autentica (o tráfego já era TCP puro); NAT simétrico impede o punching (nesse
caso use VPN ou um relay); só Linux (usa vários endereços de loopback); **a porta do Super Peer precisa ser
diferente em cada máquina** (o Super Peer escuta em `0.0.0.0` e ocuparia o endereço virtual de quem usa a
mesma porta); o app precisa estar aberto nas duas pontas.

## Abas

- **Cliente**: upload (seletor de arquivos), download (para `app/data/client/downloads/`),
  busca, guardar valor, linha de comando livre, tabela de operações e métricas do nó (`state`).
- **Super Peer**: estado ao vivo, log com filtro, reiniciar/parar/iniciar, re-registrar e listar
  os Super Peers do rendezvous. Só mostra o Super Peer desta máquina.
- **Configuração**: endereço do rendezvous, portas (Super Peer, cliente e túnel), IP anunciado, testar o servidor e reiniciar o nó.

Variáveis: `BT_BIN_DIR` (padrão `bin/`) e `BT_DATA_DIR` (padrão `app/data/`).
