#ifndef NETWORK_H
#define NETWORK_H

#include<stdint.h>

/**
 * @file network.h
 * @brief Camada de sockets IPv4/TCP e I/O de stream.
 *
 * API mínima usada por @c peer e @c superpeer para não chamar
 * @c socket / @c bind / @c listen / @c accept / @c connect / @c send / @c recv
 * diretamente. O framing de mensagem (header + payload) vive em protocol.h;
 * aqui só há transporte de bytes.
 */

/** Erro de socket ou de I/O. */
#define NET_ERROR -1
/** Operação concluída com sucesso. */
#define NET_OK 0
/** Conexão fechada pelo peer remoto (EOF em @c recv). */
#define NET_CLOSED 1

/**
 * @brief Abre um socket TCP passivo (servidor).
 *
 * Cria o socket, aplica @c SO_REUSEADDR, faz @c bind em @c INADDR_ANY
 * (todas as interfaces) e entra em @c listen com backlog 128.
 * @param port Porta TCP, host byte order.
 * @return fd de escuta, ou @c -1 em erro.
 * @note O @c bind ignora o IP do @c .conf no CP1; escuta em todas as interfaces.
 */
int net_listen(uint16_t port);

/**
 * @brief Conecta a um host IPv4 (cliente).
 * @param host_addr IPv4 em notação decimal com pontos (ex.: "127.0.0.1").
 * @param port Porta TCP, host byte order.
 * @return fd conectado, ou @c -1 em erro.
 */
int net_connect(const char* host_addr, uint16_t port);

/**
 * @brief Aceita uma conexão pendente em um socket de escuta.
 * @param listen_fd fd retornado por @c net_listen.
 * @param out_addr Recebe o endereço do remoto; o caller aloca.
 * @return fd da conexão aceita, ou @c -1 em erro.
 */
int net_accept(int listen_fd, struct sockaddr_in* out_addr);

/**
 * @brief Fecha um socket.
 * @param fd fd a fechar.
 * @return 0 em sucesso, @c -1 em erro.
 */
int net_close(int fd);

/**
 * @brief Envia @p buf_size bytes por completo, tratando @c send parcial.
 * @param fd Socket conectado.
 * @param buffer Bytes a enviar.
 * @param buf_size Quantidade de bytes.
 * @return @c NET_OK se tudo foi enviado, @c NET_ERROR em erro.
 */
int send_all(int fd, const char* buffer, uint32_t buf_size);

/**
 * @brief Recebe exatamente @p buf_size bytes, tratando @c recv parcial.
 * @param fd Socket conectado.
 * @param buffer Destino; o caller aloca ao menos @p buf_size bytes.
 * @param buf_size Quantidade exata de bytes a ler.
 * @return @c NET_OK se leu tudo, @c NET_CLOSED se o remoto fechou, @c NET_ERROR em erro.
 */
int recv_all(int fd, char* buffer, uint32_t buf_size);

#endif
