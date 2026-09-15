---
name: readme-docs
description: Use para manter o README.md e docs de visão geral na raiz sincronizados com o estado real do projeto, em pt-br. Não edita código.
---

# Mantenedor do README e documentação de alto nível

Quando usar: para manter o `README.md` (e eventuais docs de visão geral na
raiz) atualizados e coerentes com o estado real do projeto.

## Processo

1. Leia a estrutura atual do repositório (`src/`, `include/`, `scripts/`,
   `tests/`, e os `deliverable/` de cada `docs/checkpoint-*/`) para saber o
   que **já existe de fato** — não descreva funcionalidades como prontas se
   ainda não foram implementadas.
2. Releia `docs/specification/trabalho_2026_SD.pdf` para a visão geral do
   trabalho (objetivo, arquitetura peer/superpeer) ao escrever ou revisar a
   introdução do README.
3. Escreva em **pt-br**: visão geral do projeto, arquitetura (peers e
   superpeers), estrutura de diretórios, como rodar (baseado em
   `scripts/demo/` quando existir), e status dos checkpoints (o que foi
   concluído, com base nos `deliverable/` de cada checkpoint).
4. Não documente detalhes internos de implementação além do necessário para
   uma visão de alto nível — isso é papel da skill `checkpoint-docs` dentro
   de cada checkpoint.
5. Edite apenas `README.md` e docs de visão geral na raiz ou em `docs/`.
   **Nunca** edite arquivos de código (`src/`, `include/`, `tests/`,
   `scripts/`, `third_party/`, `config/`) como parte desta skill.
6. Depois de escrever, mostre o diff/resumo ao usuário. Não commite nem dê
   push automaticamente — use a skill `git-flow` quando o usuário pedir
   explicitamente.
