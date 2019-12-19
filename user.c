#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <err.h>
#include <errno.h>

struct request {
	uint32_t flags;    /* O_CLOEXEC, O_NONBLOCK */
	uint32_t wrmax;    /* single write limit (0 - disable) */
	uint32_t fd_io;    /* file to use for io */
	uint32_t fd_ioctl; /* file to use for ioctl */
};

int main()
{
	ssize_t st;
	int devfd, pipefd[2], proxyfd;
	struct request r = {};
	char buf[128];

	devfd = open("/dev/proxyfd", O_WRONLY);
	if (devfd < 0)
		err(EXIT_FAILURE, "open(/dev/proxyfd)");

	if (pipe(pipefd))
		err(EXIT_FAILURE, "pipe");

	/* Send request (should succeed). */
	r.flags = O_CLOEXEC;
	r.wrmax = 4;
	r.fd_io = pipefd[1];
	r.fd_ioctl = STDOUT_FILENO;

	proxyfd = write(devfd, &r, sizeof(r));
	if (proxyfd < 0)
		err(EXIT_FAILURE, "proxyfd");

	printf("proxy created (pipe/pty, wrmax=%d)\n", r.wrmax);

	printf("proxy isatty: %d\n", isatty(proxyfd));

	/* Another request, should fail. */
	r.fd_io = proxyfd;
	if (write(devfd, &r, sizeof(r)) >= 0)
		errno = 0;

	printf("creating proxy on top of another proxy: %s\n",
	       strerror(errno));

	/* Cleanup */
	close(devfd);

	/* Make sure that proxy properly retains member files.
	 * The pipe should remain operational. */
	if (!close(pipefd[1]))
		errno = 0;
	printf("closing pipe write end: %s\n", strerror(errno));

	/* Check that writes are accepted and truncated as expected. */
	static const char hello_world[] = "Hello, world!";
	st = write(proxyfd, hello_world, sizeof(hello_world) - 1);

	if (st >= 0)
		errno = 0;

	printf("wrote %d of %zu bytes of '%s' into proxy: %s\n",
	       (int)st, sizeof(hello_world) - 1,
	       hello_world, strerror(errno));

	/* Check that read on proxy fails, since the corresponding
	 * pipe end is wr-only */
	st = read(proxyfd, buf, sizeof(buf));
	if (st >= 0)
		errno = 0;

	printf("read on proxy yields %d byte(s): %s\n",
	       (int)st, strerror(errno));

	//* Check that data actually comes out of the other pipe end */
	st = read(pipefd[0], buf, sizeof(buf));
	if (st >= 0)
		errno = 0;

	printf("read on pipe yields %d byte(s): '%.*s': %s\n",
	       (int)st, st < 0 ? 0 : (int)st, buf, strerror(errno));

	/* Make sure that proxy properly releases member files on close.
	 * Once proxy is closed, the write end of the pipe should
	 * close as well, hence read on the pipe should report EOF. */
	if (!close(proxyfd))
		errno = 0;
	printf("closing proxy: %s\n", strerror(errno));

	st = read(pipefd[0], buf, sizeof(buf));
	if (st >= 0)
		errno = 0;
	printf("read on pipe yields %d byte(s): %s\n",
	       (int)st, strerror(errno));

	return EXIT_SUCCESS;
}
