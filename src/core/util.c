#define _GNU_SOURCE
#include "core/util.h"
#include "pifire/common.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

double pf_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

double pf_wall(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void pf_sleep_ms(unsigned ms)
{
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

size_t pf_strlcpy(char *dst, const char *src, size_t n)
{
	size_t l = strlen(src);
	if (n) {
		size_t c = l < n - 1 ? l : n - 1;
		memcpy(dst, src, c);
		dst[c] = 0;
	}
	return l;
}

size_t pf_json_escape(const char *in, char *out, size_t n)
{
	size_t w = 0;
	for (; *in && w + 7 < n; in++) {
		unsigned char c = (unsigned char)*in;
		switch (c) {
		case '"': out[w++] = '\\'; out[w++] = '"'; break;
		case '\\': out[w++] = '\\'; out[w++] = '\\'; break;
		case '\n': out[w++] = '\\'; out[w++] = 'n'; break;
		case '\r': out[w++] = '\\'; out[w++] = 'r'; break;
		case '\t': out[w++] = '\\'; out[w++] = 't'; break;
		default:
			if (c < 0x20) w += (size_t)snprintf(out + w, n - w, "\\u%04x", c);
			else out[w++] = (char)c;
		}
	}
	out[w] = 0;
	return w;
}

char *pf_read_file(const char *path, size_t *len_out)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	if (len < 0) { fclose(f); return NULL; }
	fseek(f, 0, SEEK_SET);
	char *buf = malloc((size_t)len + 1);
	if (!buf) { fclose(f); return NULL; }
	size_t rd = fread(buf, 1, (size_t)len, f);
	fclose(f);
	buf[rd] = 0;
	if (len_out) *len_out = rd;
	return buf;
}

int pf_write_file_atomic(const char *path, const void *data, size_t len)
{
	char tmp[1024];
	snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) return -errno;
	const char *p = data;
	size_t left = len;
	while (left) {
		ssize_t w = write(fd, p, left);
		if (w < 0) { if (errno == EINTR) continue; int e = errno; close(fd); unlink(tmp); return -e; }
		p += w; left -= (size_t)w;
	}
	if (fsync(fd) < 0) { int e = errno; close(fd); unlink(tmp); return -e; }
	close(fd);
	if (rename(tmp, path) < 0) { int e = errno; unlink(tmp); return -e; }
	/* fsync the directory so the rename is durable */
	char dir[1024];
	pf_strlcpy(dir, path, sizeof dir);
	char *s = strrchr(dir, '/');
	if (s) { *s = 0; int dfd = open(dir[0] ? dir : "/", O_RDONLY | O_DIRECTORY); if (dfd >= 0) { fsync(dfd); close(dfd); } }
	return 0;
}

int pf_mkdir_p(const char *path)
{
	char buf[1024];
	pf_strlcpy(buf, path, sizeof buf);
	for (char *p = buf + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			if (mkdir(buf, 0755) < 0 && errno != EEXIST) return -errno;
			*p = '/';
		}
	}
	if (mkdir(buf, 0755) < 0 && errno != EEXIST) return -errno;
	return 0;
}

bool pf_file_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

/* ---- enum names (declared in pifire/common.h) ---- */
static const char *mode_names[PF_MODE_COUNT] = {
	"Stop", "Monitor", "Prime", "Startup", "Reignite", "Smoke", "Hold", "Shutdown", "Manual", "Error"
};
const char *pf_mode_name(pf_mode m) { return (unsigned)m < PF_MODE_COUNT ? mode_names[m] : "?"; }
int pf_mode_from_name(const char *s)
{
	for (int i = 0; i < PF_MODE_COUNT; i++)
		if (!strcasecmp(s, mode_names[i])) return i;
	return -1;
}
static const char *output_names[PF_OUT_COUNT] = { "power", "fan", "auger", "igniter" };
const char *pf_output_name(pf_output o) { return (unsigned)o < PF_OUT_COUNT ? output_names[o] : "?"; }
