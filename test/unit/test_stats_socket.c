#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct writer_arg { int fd; size_t bytes; int result; };

static void *writer(void *opaque)
{
	struct writer_arg *a = opaque;
	char buf[4096];
	memset(buf, 'x', sizeof(buf));
	a->result = 0;
	for (size_t n = 0; n < a->bytes; ) {
		ssize_t rc = send(a->fd, buf,
			(a->bytes - n < sizeof(buf)) ? a->bytes - n : sizeof(buf),
			MSG_NOSIGNAL);
		if (rc <= 0) { a->result = -1; break; }
		n += (size_t)rc;
	}
	close(a->fd);
	return NULL;
}

int main(void)
{
	int sv[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	/* A small send buffer makes back-pressure deterministic. */
	int snd = 4096;
	/* Some restricted CI sandboxes reject buffer tuning; the default socket
	 * buffer still exercises the same blocking/disconnect path. */
	(void)setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
	struct writer_arg a = { sv[0], 1u << 20, 0 };
	pthread_t tid;
	assert(pthread_create(&tid, NULL, writer, &a) == 0);
	/* Deliberately do not read: this models a browser with a full TCP window. */
	usleep(20000);
	close(sv[1]);
	assert(pthread_join(tid, NULL) == 0);
	/* The writer must observe disconnect and terminate, never hang. */
	assert(a.result != 0);
	puts("stats socket slow-reader/disconnect: PASS");
	return 0;
}
