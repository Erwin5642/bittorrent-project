#include<stdint.h>
#include<stdio.h>
#include<unistd.h>
#include<arpa/inet.h>
#include "../../include/common/network.h"



int net_listen(uint16_t port){
	int net_fd;
	struct sockaddr_in address;
	socklen_t addrlen = sizeof(address);

	if((net_fd = socket(PF_INET, SOCK_STREAM, 0))<0){
		perror("net_listen:socket");
		return -1;	
	}
	int opt = 1;
	if(setsockopt(net_fd, SOL_SOCKET, SO_REUSEADDR , &opt, sizeof(opt))){
		perror("net_listen:setsockopt");
		close(net_fd);
		return -1;
	}

	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(port);

	if((bind(net_fd, (struct sockaddr*)&address, addrlen) < 0)){
		perror("net_listen:bind");
		close(net_fd);
		return -1;	
	}

	if(listen(net_fd, 128) < 0){
		perror("net_listen:listen");
		close(net_fd);
		return -1;
	}

	return net_fd;

}




int net_connect(const char* host_addr, uint16_t port){
	int net_fd;
	struct sockaddr_in address;
	socklen_t addrlen = sizeof(address);

	if((net_fd = socket(PF_INET, SOCK_STREAM, 0))<0){
		perror("net_connect:socket");
		return -1;	
	}

	address.sin_family = AF_INET;
	address.sin_port = htons(port);

	int inet_ret;
	if(( inet_ret = inet_pton(AF_INET, host_addr, &address.sin_addr)) != 1){
		if(inet_ret == 0) perror("net_connect:inet_pton # reason: Invalid input presentation");
		else perror("net_connect:inet_pton");
		close(net_fd);
		return -1;
	}

	if(connect(net_fd, (struct sockaddr*)&address, addrlen) < 0){
		perror("net_connect:connect");
		close(net_fd);
		return -1;
	}

	return net_fd;
}

int net_accept(int listen_fd, struct sockaddr_in* out_addr){
	socklen_t addrlen = sizeof(*out_addr);
	int out_fd;
	if((out_fd = accept(listen_fd, (struct sockaddr*)out_addr, &addrlen))<0){
		perror("net_accept:accept");
		return -1;
	}
	return out_fd;
}

int net_close(int fd){
	if(close(fd)<0){
		perror("net_close");
		return -1;
	}
	return 0;
}


#define NET_ERROR -1
#define NET_OK 0
#define NET_CLOSED 1

int send_all(int fd, const char* buffer, uint32_t buf_size){
	uint32_t remaining = buf_size;
	ssize_t sent;
	const char* ptr = buffer;

	while(remaining > 0){
		
		if((sent = send(fd, ptr, remaining, 0))<0){
			perror("send_all:send");
			return NET_ERROR;
		}
		remaining -= sent;
		ptr += sent;

	}
	return NET_OK;
}

int recv_all(int fd, char* buffer, uint32_t buf_size){
	uint32_t remaining = buf_size;
	ssize_t received;
	char* ptr = buffer;

	while(remaining > 0){
		
		if((received = recv(fd, ptr, remaining, 0))<=0){
			if(received == 0) return NET_CLOSED;
			perror("recv_all:received");
			return NET_ERROR;
		}

		remaining -= received;
		ptr += received;

	}
	return NET_OK;
}

