#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <arpa/inet.h>
#include <err.h>
#include <errno.h>
#include <string.h>

struct request {
	uint32_t flags;
	uint32_t cookie;
	uint32_t pipefd;
};

#define PROXYFD_DEV_PATH "/dev/proxyfd"

static void output(const char *buf, size_t size)
{
	ssize_t offset = 0;
	while (offset != size) {
		ssize_t st = write(STDOUT_FILENO, buf + offset, size - offset);
		if (st < 0) {
			if (errno == EINTR) continue;
			err(EXIT_FAILURE, "write");
		}
		offset += st;
	}
}

static void stream_forward(int fd, void(*callback)(uint32_t, ssize_t));

static void error_highlight(uint32_t hdr, ssize_t state)
{
	if (*(const char *)&hdr != '!')
		return;

	if (state)
		output("\e[31m", 5);
	else
		output("\e[0m", 4);
}

int main(int argc, char **argv)
{
	ssize_t st;
	int pipefd[2];
	int devfd;
	int proxyfd[2];
	int status;
	struct request r = { .flags = O_CLOEXEC };

	if (argc == 1) {
		printf("Usage: %s [COMMAND]...\n", argv[0]);
		return EXIT_FAILURE;
	}

	if (pipe2(pipefd, O_CLOEXEC))
		err(EXIT_FAILURE, "pipe");

	devfd = open(PROXYFD_DEV_PATH, O_WRONLY);
	if (devfd < 0)
		err(EXIT_FAILURE, "open(%s)", PROXYFD_DEV_PATH);

	r.pipefd = pipefd[1];
	r.cookie = htonl(UINT32_C(0x3e0a0000));
	st = write(devfd, &r, sizeof(r));
	if (st < 0)
		err(EXIT_FAILURE, "proxyfd");
	proxyfd[0] = (int)st;

	r.cookie = htonl(UINT32_C(0x210a0000));
	st = write(devfd, &r, sizeof(r));
	if (st < 0)
		err(EXIT_FAILURE, "proxyfd");
	proxyfd[1] = st;

	close(devfd);
	close(pipefd[1]);

	switch (fork()) {
	case -1:
		err(EXIT_FAILURE, "fork");
	case 0:
		dup2(proxyfd[0], STDOUT_FILENO);
		dup3(STDERR_FILENO, proxyfd[0], O_CLOEXEC);
		dup2(proxyfd[1], STDERR_FILENO);
		execvp(argv[1], argv + 1);
		dup2(proxyfd[0], STDERR_FILENO);
		err(EXIT_FAILURE, "exec('%s')", argv[1]);
	}

	close(proxyfd[0]);
	close(proxyfd[1]);
	stream_forward(pipefd[0], error_highlight);
	while (wait(&status) < 0) {
		if (errno == ECHILD) return EXIT_SUCCESS;
	}
	return WIFEXITED(status) ? WEXITSTATUS(status): EXIT_FAILURE;
}

static void stream_forward(int fd, void(*callback)(uint32_t, ssize_t))
{
	ssize_t st;
	ssize_t state = 0;
	uint32_t hdr;
	char buf[PIPE_BUF];

	while ((st = read(fd, buf, sizeof(buf)))) {
		ssize_t offset = 0;
		if (st < 0) {
			if (errno == EINTR) continue;
			err(EXIT_FAILURE, "read");
		}
		while (offset != st) {
			if (state > 0) {
				ssize_t sz = st - offset < state ?
				             st - offset : state;
				output(buf + offset, sz);
				offset += sz;
				if ((state -= sz)) continue;
			}
			if (!state && callback)
				callback(hdr, 0);
			if (st - offset < 4 + state) {
				memcpy((char *)&hdr - state, buf + offset, st - offset);
				state -= st - offset;
				break;
			}
			memcpy((char *)&hdr - state, buf + offset, 4 + state);
			offset += 4 + state;
			state = ntohl(hdr) & 0xffff;
			if (callback)
				callback(hdr, state);
		}
	}
}
