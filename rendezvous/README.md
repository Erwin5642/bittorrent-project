# Servidor de rendezvous

Guarda o endereço público de cada Super Peer que se registra e devolve endereços de outros
Super Peers para o recém-chegado usar como `bootstrap`. Um processo em C, sem dependências.

## Protocolo

Texto sobre TCP, **uma conexão = um comando** (porta padrão 7000):

| Comando | Resposta |
|---|---|
| `REGISTER <porta> [ip]` | `OK <ip>:<porta>`, até 3 linhas `PEER <ip>:<porta>` (os mais antigos, exceto o próprio), `END` |
| `UNREGISTER <porta> [ip]` | `OK` |
| `LIST` | `PEER <ip>:<porta>` para cada registro, `END` |
| `PING` | `PONG` |
| erro | `ERR <motivo>` |

O IP registrado é o que o servidor enxerga na conexão; a porta é a de escuta do Super Peer,
informada por ele. O `[ip]` opcional substitui o IP visto (atrás de NAT ou proxy). `REGISTER`
também renova o registro: sem renovação, ele expira após o TTL (padrão 60 s).

## Uso por script

```bash
rendezvous/rdv.sh 192.168.0.10:7000 register 5100   # OK 192.168.0.20:5100 / PEER ...
rendezvous/rdv.sh 192.168.0.10:7000 list
rendezvous/rdv.sh 192.168.0.10:7000 unregister 5100
```

Só precisa de bash (usa `/dev/tcp`). Saída: a resposta do servidor sem o `END`. Código de
saída: `0` ok, `1` o servidor respondeu `ERR`, `2` uso errado, `3` sem conexão ou timeout.

## Executar

```bash
make -C rendezvous && rendezvous/rendezvous -p 7000 -t 60 -v
```

Docker (use `--network host`: com a rede bridge, um cliente na mesma máquina do servidor
aparece com o IP do gateway do Docker em vez do IP dele):

```bash
docker build -t bt-rendezvous rendezvous/
docker run --rm --network host bt-rendezvous                 # porta 7000
docker run --rm -p 7000:7000 bt-rendezvous -t 120 -v         # sem host network
```

## Deploy na AWS

`deploy.sh` e `aws-template.yaml` publicam o servidor numa instância EC2 (Amazon Linux 2023, `t3.micro`) com **IP
elástico**, imagem no ECR e logs no CloudWatch. Precisa de `aws` CLI v2 autenticada e `docker`.

```bash
rendezvous/deploy.sh up        # ECR + imagem + stack CloudFormation; imprime host:porta
rendezvous/deploy.sh status    # endereço + PING no servidor
rendezvous/deploy.sh logs      # log recente (CloudWatch)
rendezvous/deploy.sh update    # imagem nova e reinício do container, sem recriar a instância
rendezvous/deploy.sh destroy [--purge]
```

O endereço impresso (`<ip-elástico>:7000`) é o que vai na aba Configuração do app. Variáveis:
`AWS_REGION`, `STACK`, `PORT`, `TTL`, `ALLOWED_CIDR` (padrão `0.0.0.0/0`), `INSTANCE_TYPE`.

Escolhas: EC2 com `--network host` (o servidor precisa do IP real de cada Super Peer; NAT,
proxy ou Fargate sem preservação de IP registrariam o endereço errado); IP elástico para o
endereço sobreviver a reinícios; sem SSH, o acesso é pelo Session Manager (SSM); usa a VPC
padrão da região. O servidor guarda o estado só em memória: reiniciar a instância esvazia a
lista até os apps renovarem (a cada 20 s). Atenção: o servidor não tem autenticação; qualquer
um que alcance a porta pode registrar endereços. Restrinja com `ALLOWED_CIDR` se der.

## Limitações

Atende uma conexão por vez, com timeout de 2 s, e guarda no máximo 256 registros. O servidor
não verifica se o Super Peer está acessível: só confia no registro e no TTL.
