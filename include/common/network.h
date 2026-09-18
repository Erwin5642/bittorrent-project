#ifndef NETWORK_H
#define NETWORK_H

#include<stdint.h>

#define NET_ERROR -1
#define NET_OK 0
#define NET_CLOSED 1

int net_listen(uint16_t port);
int net_connect(const char* host_addr, uint16_t port);
int net_accept(int listen_fd, struct sockaddr_in* out_addr);
int net_close(int fd);
int send_all(int fd, const char* buffer, uint32_t buf_size);
int recv_all(int fd, char* buffer, uint32_t buf_size);

#endif
