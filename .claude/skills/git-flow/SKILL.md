---
name: git-flow
description: Use ao criar branches, commits ou Pull Requests neste repositório (git, branch, commit, PR, merge). Modelo main/master como release e dev como integração, branches de tarefa (não por checkpoint), Conventional Commits, e nunca commitar/dar push sem pedido explícito do usuário.
---

# Fluxo de git do projeto

Quando usar: ao criar branches, fazer commits, ou abrir PRs para qualquer
mudança neste repositório — geralmente documentação, eventualmente código se
o usuário pedir explicitamente.

## Branches principais

- `master` (a branch default do repositório, papel de "main") — branch de
  **release**: contém, em cada momento, a implementação/documentação final já
  fechada de um checkpoint. Só recebe merge vindo de `dev`, via PR, nunca
  recebe merge direto de uma branch de tarefa.
- `dev` — branch de **integração**: é o alvo (destino) de toda branch de
  tarefa. Toda branch de tarefa nasce a partir de `dev` atualizado e volta
  para `dev` via PR.

Fluxo resumido: `branch de tarefa → dev → master`. `master` avança apenas
quando `dev` chega a um estado que representa o fechamento de um checkpoint
(um "release").

## Branches de tarefa

- Criadas **por tarefa**, sem mapeamento fixo 1:1 com checkpoints. Nomeie
  como `<tipo>/<slug-curto>`, com `<tipo>` do vocabulário do Conventional
  Commits: `docs`, `feat`, `fix`, `chore`, `test`, `refactor`.
- O slug pode ser em pt-br ou inglês, curto e descritivo, em kebab-case.
  Exemplos: `docs/plano-checkpoint-03`, `docs/atualiza-readme`,
  `chore/organiza-pastas-checkpoint-05`.
- Crie a branch a partir de **`dev` atualizado** (não de `master`). Não
  trabalhe direto em `dev` ou `master` para mudanças que não sejam triviais e
  já combinadas com o usuário.

## Commits

- Siga Conventional Commits: `tipo(escopo opcional): resumo`. O resumo pode
  ser em pt-br. Use o checkpoint ou a área como escopo quando fizer sentido,
  ex.: `docs(checkpoint-03): adiciona plano de implementação do Chord`.
- Foque o resumo no "porquê" da mudança, não numa lista do que foi alterado —
  siga as diretrizes padrão de mensagens de commit curtas e objetivas.
- **Nunca commite ou dê push sem o usuário pedir isso explicitamente nesta
  conversa**, mesmo estando dentro desta skill. Prepare e mostre o diff,
  espere confirmação.
- Nunca use comandos destrutivos (`push --force`, `reset --hard`, `clean -f`,
  apagar branches) sem confirmação explícita do usuário.

## Pull Requests

- Para o template de título/corpo do PR (e para issues relacionadas), ver a
  skill `github-prs` (e `github-issues` para abrir a issue em si). Esta
  seção aqui cobre apenas a mecânica de branch/base.
- Antes de abrir qualquer PR, confirme que a mudança está numa branch de
  tarefa dedicada (`<tipo>/<slug-curto>`). Se o que o usuário quer publicar
  estiver em `dev` (ou em qualquer branch que não seja uma branch de tarefa)
  — por commits feitos ali diretamente, ou por alterações ainda não
  commitadas — **primeiro crie a branch de tarefa e mova a mudança para
  ela**, e só então abra o PR:
  - Alterações ainda não commitadas: crie a branch a partir de `dev`
    (`git checkout -b <tipo>/<slug>`) — o working tree acompanha a troca de
    branch automaticamente.
  - Commits já feitos diretamente em `dev`: crie a branch apontando para o
    commit atual (`git branch <tipo>/<slug>`) e, se esses commits ainda não
    foram enviados ao remoto, volte `dev` para o commit anterior a eles
    (`git reset --hard <commit-anterior>`) para manter `dev` limpo — peça
    confirmação ao usuário antes desse reset, mesmo sendo só local. Se os
    commits já foram enviados ao remoto, pare e avise o usuário em vez de
    reescrever histórico compartilhado.
  - Nunca abra um PR a partir de `dev` ou `master` representando uma única
    funcionalidade/tarefa — isso deve sempre virar uma branch de tarefa
    antes.
- **Branch de tarefa → `dev`**: o caminho normal para qualquer mudança
  (documentação, ou código quando pedido explicitamente). Título curto;
  descrição em pt-br explicando o quê e por quê, com uma checklist simples
  (ex.: "documentação revisada", "nenhum arquivo de código alterado", quando
  aplicável). Use `gh pr create --base dev`.
- **`dev` → `master`**: só quando o checkpoint correspondente está
  fechado/o deliverable finalizado — é o "release" daquele checkpoint para
  `master`. Título no formato `release: checkpoint-0N - <resumo>`, descrição
  lista o que foi entregue. Use `gh pr create --base master --head dev`.
- **Nunca** abra PR de uma branch de tarefa direto para `master`.
- PRs originados por tarefas de documentação normalmente não tocam em
  `src/`, `include/`, `tests/`, `scripts/`, `third_party/` ou `config/`. Se o
  diff tocar nessas pastas, pare e confirme com o usuário antes de seguir —
  ver a regra de "código é somente leitura" no contexto geral do projeto.
- Não dê push nem abra o PR sem o usuário pedir explicitamente.
