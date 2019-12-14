/* basic test suite for the kernel module */
#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/socket.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <err.h>
#include <errno.h>

struct request {
	uint32_t flags;    /* O_CLOEXEC, O_NONBLOCK */
	uint32_t cookie;
	uint32_t pipefd;
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
	r.cookie = UINT32_C(0x20202020);
	r.pipefd = pipefd[1];

	proxyfd = write(devfd, &r, sizeof(r));
	if (proxyfd < 0)
		err(EXIT_FAILURE, "proxyfd");

	printf("proxy created\n");

	printf("proxy isatty: %d\n", isatty(proxyfd));

	/* wrong end of pipe */
	r.pipefd = pipefd[0];
	if (write(devfd, &r, sizeof(r)) >= 0)
		errno = 0;

	printf("create with wrong end of pipe: %s\n",
	       strerror(errno));


	/* unexpected file kind (1) */
	r.pipefd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (write(devfd, &r, sizeof(r)) >= 0)
		errno = 0;

	printf("create with unexpected file kind (1): %s\n",
	       strerror(errno));

	/* unexpected file kind (2) */
	r.pipefd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (write(devfd, &r, sizeof(r)) >= 0)
		errno = 0;

	printf("create with unexpected file kind (2): %s\n",
	       strerror(errno));

	/* Cleanup */
	close(devfd);

	/* Make sure that proxy properly retains member files.
	 * The pipe should remain operational. */
	if (!close(pipefd[1]))
		errno = 0;
	printf("closing pipe write end: %s\n", strerror(errno));

	/* Check that writes are accepted. */
	static const char m1[] = "Hello, world!",
	                  m2[] = "lorem ipsum dolor sit amet";

	st = write(proxyfd, m1, sizeof(m1) - 1);
	if (st >= 0)
		errno = 0;
	printf("wrote %d of %zu bytes of '%s' into proxy: %s\n",
	       (int)st, sizeof(m1) - 1,
	       m1, strerror(errno));

	st = write(proxyfd, m2, sizeof(m2) - 1);
	if (st >= 0)
		errno = 0;
	printf("wrote %d of %zu bytes of '%s' into proxy: %s\n",
	       (int)st, sizeof(m2) - 1,
	       m2, strerror(errno));

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
