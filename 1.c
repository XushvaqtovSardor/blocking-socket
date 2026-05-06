#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define BACKLOG 128
#define MAX_MSG_SIZE (1024 * 1024)
#define IO_TIMEOUT_SEC 5

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int sig) {
	(void)sig;
	g_stop = 1;
}

static int set_common_sockopts(int fd) {
	int yes = 1;
	struct timeval tv;

	if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes)) != 0) {
		return -1;
	}

	tv.tv_sec = IO_TIMEOUT_SEC;
	tv.tv_usec = 0;
	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
		return -1;
	}
	if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
		return -1;
	}

	return 0;
}

static ssize_t recv_exact(int fd, void *buf, size_t n) {
	size_t off = 0;
	while (off < n) {
		ssize_t r = recv(fd, (char *)buf + off, n - off, 0);
		if (r == 0) {
			return 0;
		}
		if (r < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}
		off += (size_t)r;
	}
	return (ssize_t)off;
}

static ssize_t send_all(int fd, const void *buf, size_t n) {
	size_t off = 0;
	while (off < n) {
		ssize_t r = send(fd, (const char *)buf + off, n - off, 0);
		if (r < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}
		off += (size_t)r;
	}
	return (ssize_t)off;
}

static int send_frame(int fd, const void *buf, uint32_t len) {
	uint32_t net_len = htonl(len);
	if (send_all(fd, &net_len, sizeof(net_len)) < 0) {
		return -1;
	}
	if (len > 0 && send_all(fd, buf, len) < 0) {
		return -1;
	}
	return 0;
}

static int recv_frame(int fd, unsigned char **out, uint32_t *out_len) {
	uint32_t net_len = 0;
	ssize_t r = recv_exact(fd, &net_len, sizeof(net_len));
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
		return (int)r;
	}

	buf[len] = '\0';
	*out = buf;
	*out_len = len;
	return 1;
}

static int make_server_socket(const char *host, const char *port) {
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	struct addrinfo *p = NULL;
	int fd = -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	if (getaddrinfo(host, port, &hints, &res) != 0) {
		return -1;
	}

	for (p = res; p != NULL; p = p->ai_next) {
		int yes = 1;
		fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (fd < 0) {
			continue;
		}
		(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
		if (bind(fd, p->ai_addr, p->ai_addrlen) == 0) {
			if (listen(fd, BACKLOG) == 0) {
				freeaddrinfo(res);
				return fd;
			}
		}
		close(fd);
		fd = -1;
	}

	freeaddrinfo(res);
	return -1;
}

static int connect_to_host(const char *host, const char *port) {
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	struct addrinfo *p = NULL;
	int fd = -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if (getaddrinfo(host, port, &hints, &res) != 0) {
		return -1;
	}

	for (p = res; p != NULL; p = p->ai_next) {
		fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (fd < 0) {
			continue;
		}
		if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
			freeaddrinfo(res);
			return fd;
		}
		close(fd);
		fd = -1;
	}

	freeaddrinfo(res);
	return -1;
}

static void *client_thread(void *arg) {
	int fd = *(int *)arg;
	free(arg);

	if (set_common_sockopts(fd) != 0) {
		close(fd);
		return NULL;
	}

	while (!g_stop) {
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

	close(fd);
	return NULL;
}

static int run_server(const char *host, const char *port) {
	int srv = make_server_socket(host, port);
	if (srv < 0) {
		fprintf(stderr, "failed to bind\n");
		return 1;
	}

	(void)signal(SIGINT, on_sigint);
	printf("listening on %s:%s\n", host, port);

	while (!g_stop) {
		struct sockaddr_storage addr;
		socklen_t addr_len = sizeof(addr);
		int fd = accept(srv, (struct sockaddr *)&addr, &addr_len);
		if (fd < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}

		int *fdp = (int *)malloc(sizeof(int));
		if (!fdp) {
			close(fd);
			continue;
		}
		*fdp = fd;

		pthread_t tid;
		if (pthread_create(&tid, NULL, client_thread, fdp) == 0) {
			pthread_detach(tid);
		} else {
			close(fd);
			free(fdp);
		}
	}

	close(srv);
	return 0;
}

static int run_client(const char *host, const char *port, const char *msg) {
	int fd = connect_to_host(host, port);
	if (fd < 0) {
		fprintf(stderr, "failed to connect\n");
		return 1;
	}
	if (set_common_sockopts(fd) != 0) {
		close(fd);
		return 1;
	}

	uint32_t len = (uint32_t)strlen(msg);
	if (len > MAX_MSG_SIZE) {
		fprintf(stderr, "message too large\n");
		close(fd);
		return 1;
	}

	if (send_frame(fd, msg, len) != 0) {
		fprintf(stderr, "send failed\n");
		close(fd);
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

	close(fd);
	return 0;
}

int main(int argc, char **argv) {
	if (argc < 4) {
		fprintf(stderr, "usage: %s server <host> <port>\n", argv[0]);
		fprintf(stderr, "   or: %s client <host> <port> <message>\n", argv[0]);
		return 1;
	}

	if (strcmp(argv[1], "server") == 0) {
		return run_server(argv[2], argv[3]);
	}

	if (strcmp(argv[1], "client") == 0) {
		if (argc < 5) {
			fprintf(stderr, "missing message\n");
			return 1;
		}
		return run_client(argv[2], argv[3], argv[4]);
	}

	fprintf(stderr, "unknown mode\n");
	return 1;
}
