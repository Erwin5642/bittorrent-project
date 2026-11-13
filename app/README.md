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
3. Grava `app/data/superpeer/sp.conf`: `ip=` é o IP público devolvido; `bootstrap=` só aparece
   se o servidor devolveu algum Super Peer.
   **Sem rendezvous configurado, ou se ele não responder, nada é iniciado**: o app abre a aba
   Configuração com o motivo.
4. Sobe o Super Peer e o cliente (`bin/client --interactive`, falando com o Super Peer local).
5. Renova o registro a cada 20 s. Ao sair (`Ctrl+Q`), faz `unregister` e encerra os processos.

## Abas

- **Cliente**: upload (seletor de arquivos), download (para `app/data/client/downloads/`),
  busca, guardar valor, linha de comando livre, tabela de operações e métricas do nó (`state`).
- **Super Peer**: estado ao vivo, log com filtro, reiniciar/parar/iniciar, re-registrar e listar
  os Super Peers do rendezvous. Só mostra o Super Peer desta máquina.
- **Configuração**: endereço do rendezvous, portas, IP anunciado, testar o servidor e reiniciar o nó.

Variáveis: `BT_BIN_DIR` (padrão `bin/`) e `BT_DATA_DIR` (padrão `app/data/`).
