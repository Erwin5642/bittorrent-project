#ifndef TEST_UTILS_H
#define TEST_UTILS_H

/**
 * @file test_utils.h
 * @brief Helpers compartilhados pelos testes unitários.
 */

/**
 * @brief Conta um assert e registra falha em stderr se @p cond for falso.
 * @param cond Condição esperada verdadeira.
 * @param msg Mensagem impressa em caso de falha.
 */
void expect(int cond, const char *msg);

/**
 * @brief Imprime o resumo e devolve o código de saída do processo de teste.
 * @return 0 se todos passaram, 1 se houve falha.
 */
int test_report(void);

#endif
