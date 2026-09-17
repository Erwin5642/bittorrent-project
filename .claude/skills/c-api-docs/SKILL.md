---
name: c-api-docs
description: Documenta a API C/C++ pública com Doxygen (blocos /** */) em pt-br nos headers. Use ao criar ou alterar tipos/funções em include/ ou src/, ou quando o usuário pedir para documentar código C/C++.
---

# Documentação Doxygen da API C/C++

Quando usar: novo header, novo tipo/função públicos, ou pedido para documentar código C/C++.

## Regras

- Comentários em **pt-br**; identificadores (`node_id_t`, `@param ipv4`) em inglês.
- API pública só no **header** (`include/**/*.h`). O `.c` não replica `@brief`/`@param`.
- No `.c`: comentário curto só para lógica não óbvia.
- Blocos `/** ... */`. Tags mínimas: `@file`, `@brief`, `@param`, `@return` ou `@retval`, `@note` se endianness/fio importar.
- Documentar macros, `typedef`/`struct`/`enum` e funções. Não documentar include guards.
- Não inventar campos da spec. Não gerar `Doxyfile`/HTML a menos que o usuário peça.
- clangd no nvim lê essas tags no hover; por isso a declaração documentada é a do header.

## Exemplo

```c
/**
 * @file node.h
 * @brief Identidade do nó, UUID e configuração local.
 */

/**
 * @brief Identificador SHA-256 do nó (32 bytes).
 */
typedef struct {
  uint8_t bytes[NODE_ID_SIZE]; /**< Digest em bruto, não string hex. */
} node_id_t;

/**
 * @brief Calcula SHA-256 de um buffer.
 * @param in Bytes de entrada.
 * @param len Tamanho de @p in em bytes.
 * @param out Digest de 32 bytes; o caller aloca.
 * @return 1 em sucesso, 0 se o OpenSSL falhar.
 */
int sha256(const uint8_t *in, const uint32_t len, uint8_t out[NODE_ID_SIZE]);
```

## Processo

1. Leia o header atual; não mude assinaturas só para documentar.
2. Acrescente `@file` no topo (depois dos includes de sistema, antes da API, ou imediatamente após o include guard — um único `@file` por header).
3. Um bloco por macro, tipo e função.
4. Mostre o resumo ao usuário. Não commite sem pedido explícito.
