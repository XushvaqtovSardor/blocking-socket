#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "Ws2_32.lib")
#endif
typedef SOCKET socket_t;
#define CLOSESOCK closesocket
#define INVALID_SOCK INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int socket_t;
#define CLOSESOCK close
#define INVALID_SOCK (-1)
#endif

#define BACKLOG 64
#define MAX_MSG_SIZE (1024 * 1024)
#define IO_TIMEOUT_MS 5000

static int init_network(void) {
#ifdef _WIN32
	WSADATA wsa;
	return WSAStartup(MAKEWORD(2, 2), &wsa);
#else
	return 0;
#endif
}

static void cleanup_network(void) {
#ifdef _WIN32
	WSACleanup();
#endif
}

static int set_common_sockopts(socket_t fd) {
	int yes = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char *)&yes, sizeof(yes)) != 0) {
		return -1;
	}

#ifdef _WIN32
	int to = IO_TIMEOUT_MS;
	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to)) != 0) {
		return -1;
	}
	if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof(to)) != 0) {
		return -1;
	}
#else
	struct timeval tv;
	tv.tv_sec = IO_TIMEOUT_MS / 1000;
	tv.tv_usec = (IO_TIMEOUT_MS % 1000) * 1000;
	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
		return -1;
	}
	if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
		return -1;
	}
#endif

	return 0;
}

static int recv_exact(socket_t fd, void *buf, size_t n) {
	size_t off = 0;
	while (off < n) {
		int r = (int)recv(fd, (char *)buf + off, (int)(n - off), 0);
		if (r == 0) {
			return 0;
		}
		if (r < 0) {
#ifndef _WIN32
			if (errno == EINTR) {
				continue;
			}
#endif
			return -1;
		}
		off += (size_t)r;
	}
	return (int)off;
}

static int send_all(socket_t fd, const void *buf, size_t n) {
	size_t off = 0;
	while (off < n) {
		int r = (int)send(fd, (const char *)buf + off, (int)(n - off), 0);
		if (r < 0) {
#ifndef _WIN32
			if (errno == EINTR) {
				continue;
			}
#endif
			return -1;
		}
		off += (size_t)r;
	}
	return (int)off;
}

static int send_frame(socket_t fd, const void *buf, uint32_t len) {
	uint32_t net_len = htonl(len);
	if (send_all(fd, &net_len, sizeof(net_len)) < 0) {
		return -1;
	}
	if (len > 0 && send_all(fd, buf, len) < 0) {
		return -1;
	}
	return 0;
}

static int recv_frame(socket_t fd, unsigned char **out, uint32_t *out_len) {
	uint32_t net_len = 0;
	int r = recv_exact(fd, &net_len, sizeof(net_len));
	if (r == 0) {
		return 0;
	}
	if (r < 0) {
		return -1;
	}

	uint32_t len = ntohl(net_len);
	if (len > MAX_MSG_SIZE) {
		return -2;
	}

	unsigned char *buf = (unsigned char *)malloc(len + 1);
	if (!buf) {
		return -3;
	}

	r = recv_exact(fd, buf, len);
	if (r <= 0) {
		free(buf);
		return r;
	}

	buf[len] = '\0';
	*out = buf;
	*out_len = len;
	return 1;
}

static socket_t make_server_socket(const char *host, const char *port) {
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	struct addrinfo *p = NULL;
	socket_t fd = INVALID_SOCK;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	if (getaddrinfo(host, port, &hints, &res) != 0) {
		return INVALID_SOCK;
	}

	for (p = res; p != NULL; p = p->ai_next) {
		int yes = 1;
		fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (fd == INVALID_SOCK) {
			continue;
		}
		(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
		if (bind(fd, p->ai_addr, (int)p->ai_addrlen) == 0) {
			if (listen(fd, BACKLOG) == 0) {
				freeaddrinfo(res);
				return fd;
			}
		}
		CLOSESOCK(fd);
		fd = INVALID_SOCK;
	}

	freeaddrinfo(res);
	return INVALID_SOCK;
}

static socket_t connect_to_host(const char *host, const char *port) {
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	struct addrinfo *p = NULL;
	socket_t fd = INVALID_SOCK;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if (getaddrinfo(host, port, &hints, &res) != 0) {
		return INVALID_SOCK;
	}

	for (p = res; p != NULL; p = p->ai_next) {
		fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (fd == INVALID_SOCK) {
			continue;
		}
		if (connect(fd, p->ai_addr, (int)p->ai_addrlen) == 0) {
			freeaddrinfo(res);
			return fd;
		}
		CLOSESOCK(fd);
		fd = INVALID_SOCK;
	}

	freeaddrinfo(res);
	return INVALID_SOCK;
}

static void log_peer(const struct sockaddr *addr, socklen_t len) {
	char host[NI_MAXHOST];
	char serv[NI_MAXSERV];
	if (getnameinfo(addr, len, host, sizeof(host), serv, sizeof(serv),
					NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
		printf("client connected: %s:%s\n", host, serv);
	}
}

static void handle_client(socket_t fd) {
	if (set_common_sockopts(fd) != 0) {
		CLOSESOCK(fd);
		return;
	}

	while (1) {
		unsigned char *msg = NULL;
		uint32_t len = 0;
		int rc = recv_frame(fd, &msg, &len);
		if (rc == 0) {
			break;
		}
		if (rc == -2) {
			const char *err = "ERR: message too large";
			(void)send_frame(fd, err, (uint32_t)strlen(err));
			break;
		}
		if (rc < 0) {
			break;
		}

		if (len > 0) {
			fwrite(msg, 1, len, stdout);
			fputc('\n', stdout);
			fflush(stdout);
		}

		(void)send_frame(fd, "OK", 2);
		free(msg);
	}

	CLOSESOCK(fd);
}

static int run_server(const char *host, const char *port) {
	socket_t srv = make_server_socket(host, port);
	if (srv == INVALID_SOCK) {
		fprintf(stderr, "failed to bind\n");
		return 1;
	}

	printf("listening on %s:%s\n", host, port);

	while (1) {
		struct sockaddr_storage addr;
		socklen_t addr_len = sizeof(addr);
		socket_t fd = accept(srv, (struct sockaddr *)&addr, &addr_len);
		if (fd == INVALID_SOCK) {
			continue;
		}
		log_peer((struct sockaddr *)&addr, addr_len);
		handle_client(fd);
	}

	CLOSESOCK(srv);
	return 0;
}

static int run_client(const char *host, const char *port, const char *msg) {
	socket_t fd = connect_to_host(host, port);
	if (fd == INVALID_SOCK) {
		fprintf(stderr, "failed to connect\n");
		return 1;
	}
	if (set_common_sockopts(fd) != 0) {
		CLOSESOCK(fd);
		return 1;
	}

	uint32_t len = (uint32_t)strlen(msg);
	if (len > MAX_MSG_SIZE) {
		fprintf(stderr, "message too large\n");
		CLOSESOCK(fd);
		return 1;
	}

	if (send_frame(fd, msg, len) != 0) {
		fprintf(stderr, "send failed\n");
		CLOSESOCK(fd);
		return 1;
	}

	unsigned char *reply = NULL;
	uint32_t reply_len = 0;
	int rc = recv_frame(fd, &reply, &reply_len);
	if (rc > 0) {
		fwrite(reply, 1, reply_len, stdout);
		fputc('\n', stdout);
		free(reply);
	}

	CLOSESOCK(fd);
	return 0;
}

int main(int argc, char **argv) {
	if (init_network() != 0) {
		fprintf(stderr, "network init failed\n");
		return 1;
	}

	if (argc < 4) {
		fprintf(stderr, "usage: %s server <host> <port>\n", argv[0]);
		fprintf(stderr, "   or: %s client <host> <port> <message>\n", argv[0]);
		cleanup_network();
		return 1;
	}

	int rc = 0;
	if (strcmp(argv[1], "server") == 0) {
		rc = run_server(argv[2], argv[3]);
	} else if (strcmp(argv[1], "client") == 0) {
		if (argc < 5) {
			fprintf(stderr, "missing message\n");
			cleanup_network();
			return 1;
		}
		rc = run_client(argv[2], argv[3], argv[4]);
	} else {
		fprintf(stderr, "unknown mode\n");
		rc = 1;
	}

	cleanup_network();
	return rc;
}
