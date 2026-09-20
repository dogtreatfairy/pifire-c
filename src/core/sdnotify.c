#define _GNU_SOURCE
#include <stddef.h>
#include "core/sdnotify.h"
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

void pf_sd_notify(const char *state)
{
	const char *path = getenv("NOTIFY_SOCKET");
	if (!path || !*path) return;
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	size_t plen = strlen(path);
	if (plen >= sizeof sa.sun_path) return;
	memcpy(sa.sun_path, path, plen + 1);
	if (sa.sun_path[0] == '@') sa.sun_path[0] = 0; /* abstract namespace */
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return;
	sendto(fd, state, strlen(state), MSG_NOSIGNAL, (struct sockaddr *)&sa,
	       (socklen_t)(offsetof(struct sockaddr_un, sun_path) + plen));
	close(fd);
}

int pf_sd_watchdog_usec(void)
{
	const char *s = getenv("WATCHDOG_USEC");
	return s ? atoi(s) : 0;
}
