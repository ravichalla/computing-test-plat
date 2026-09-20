// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace test suite for the valdev kernel module.
 *
 * Output is TAP (Test Anything Protocol) so it plugs into CI tooling:
 *     ok 1 - open_close
 *     not ok 2 - version_matches_abi # expected 65536, got 0
 *
 * Exit status: 0 = all passed, 1 = a test failed, 2 = device could not be opened.
 * Usage: ./test_valdev [/dev/valdev]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "valdev_ioctl.h"

static const char *dev_path = "/dev/" VALDEV_NAME;
static char why[256];  /* failure reason for the current test */

#define CHECK(cond, ...)                                    \
	do {                                                    \
		if (!(cond)) {                                      \
			snprintf(why, sizeof(why), __VA_ARGS__);        \
			return -1;                                      \
		}                                                   \
	} while (0)

/* A pointer that is guaranteed unmapped. volatile stops the compiler from
 * folding it into a constant and warning about the (intentional) bad access. */
static void *bad_ptr(void)
{
	volatile uintptr_t v = 1;

	return (void *)v;
}

static int dev_open(void)
{
	return open(dev_path, O_RDWR);
}

static int dev_reset(int fd)
{
	return ioctl(fd, VALDEV_IOC_RESET);
}

static int get_stats(int fd, struct valdev_stats *s)
{
	memset(s, 0xAA, sizeof(*s));   /* poison so an untouched field is obvious */
	return ioctl(fd, VALDEV_IOC_GET_STATS, s);
}

/* ------------------------------------------------------------------ tests */

static int t_open_close(void)
{
	int fd = dev_open();

	CHECK(fd >= 0, "open failed: %s", strerror(errno));
	CHECK(close(fd) == 0, "close failed: %s", strerror(errno));
	return 0;
}

static int t_version_matches_abi(void)
{
	uint32_t v = 0;
	int fd = dev_open();

	CHECK(fd >= 0, "open failed");
	CHECK(ioctl(fd, VALDEV_IOC_GET_VERSION, &v) == 0, "ioctl: %s", strerror(errno));
	CHECK(v == VALDEV_ABI_VERSION, "expected 0x%x, got 0x%x", VALDEV_ABI_VERSION, v);
	close(fd);
	return 0;
}

static int t_write_read_roundtrip(void)
{
	const char msg[] = "hello valdev";
	char buf[64] = { 0 };
	int fd = dev_open();

	CHECK(fd >= 0, "open failed");
	CHECK(write(fd, msg, sizeof(msg)) == (ssize_t)sizeof(msg), "short write");
	CHECK(lseek(fd, 0, SEEK_SET) == 0, "lseek(0) failed");
	CHECK(read(fd, buf, sizeof(buf)) == (ssize_t)sizeof(msg), "wrong read length");
	CHECK(memcmp(buf, msg, sizeof(msg)) == 0, "data mismatch: '%s'", buf);
	close(fd);
	return 0;
}

static int t_read_empty_is_eof(void)
{
	char buf[16];
	int fd = dev_open();

	CHECK(fd >= 0, "open failed");
	CHECK(read(fd, buf, sizeof(buf)) == 0, "expected EOF on an empty buffer");
	close(fd);
	return 0;
}

static int t_partial_reads_follow_offset(void)
{
	char buf[128] = { 0 };
	int fd = dev_open();

	CHECK(fd >= 0, "open failed");
	CHECK(write(fd, "0123456789", 10) == 10, "write failed");
	CHECK(lseek(fd, 4, SEEK_SET) == 4, "lseek failed");
	CHECK(read(fd, buf, 3) == 3 && memcmp(buf, "456", 3) == 0, "first partial read wrong");
	CHECK(read(fd, buf, 100) == 3 && memcmp(buf, "789", 3) == 0, "read must stop at end of valid data");
	CHECK(read(fd, buf, 1) == 0, "expected EOF after last byte");
	close(fd);
	return 0;
}

static int t_write_straddling_end_is_short(void)
{
	char big[64];
	int fd = dev_open();

	memset(big, 'x', sizeof(big));
	CHECK(fd >= 0, "open failed");
	CHECK(lseek(fd, VALDEV_BUF_SIZE - 6, SEEK_SET) == VALDEV_BUF_SIZE - 6, "lseek failed");
	CHECK(write(fd, big, 20) == 6, "expected a 6-byte short write at the end of the buffer");
	errno = 0;
	CHECK(write(fd, big, 1) == -1 && errno == ENOSPC, "expected ENOSPC once the buffer is full, got %s",
	      strerror(errno));
	close(fd);
	return 0;
}

static int t_write_at_capacity_is_enospc(void)
{
	int fd = dev_open();

	CHECK(fd >= 0, "open failed");
	CHECK(lseek(fd, VALDEV_BUF_SIZE, SEEK_SET) == VALDEV_BUF_SIZE, "seek to capacity should be allowed");
	errno = 0;
	CHECK(write(fd, "z", 1) == -1 && errno == ENOSPC, "expected ENOSPC, got %s", strerror(errno));
	close(fd);
	return 0;
}

static int t_llseek_validates_offsets(void)
{
	int fd = dev_open();

	CHECK(fd >= 0, "open failed");
	errno = 0;
	CHECK(lseek(fd, VALDEV_BUF_SIZE + 1, SEEK_SET) == -1 && errno == EINVAL, "seek past capacity must be EINVAL");
	errno = 0;
	CHECK(lseek(fd, -1, SEEK_SET) == -1 && errno == EINVAL, "negative seek must be EINVAL");
	CHECK(write(fd, "0123456789", 10) == 10, "write failed");
	CHECK(lseek(fd, 0, SEEK_END) == 10, "SEEK_END should land on the valid length");
	CHECK(lseek(fd, -4, SEEK_CUR) == 6, "SEEK_CUR relative seek wrong");
	close(fd);
	return 0;
}

static int t_counter_accumulates(void)
{
	uint64_t v = 5;
	int fd = dev_open();

	CHECK(fd >= 0, "open failed");
	CHECK(ioctl(fd, VALDEV_IOC_ADD_COUNTER, &v) == 0 && v == 5, "first add: got %llu", (unsigned long long)v);
	v = 7;
	CHECK(ioctl(fd, VALDEV_IOC_ADD_COUNTER, &v) == 0 && v == 12, "second add: got %llu", (unsigned long long)v);
	close(fd);
	return 0;
}

static int t_reset_clears_state(void)
{
	struct valdev_stats s;
	uint64_t v = 9;
	char c;
	int fd = dev_open();

	CHECK(fd >= 0, "open failed");
	CHECK(write(fd, "abc", 3) == 3, "write failed");
	CHECK(ioctl(fd, VALDEV_IOC_ADD_COUNTER, &v) == 0, "add failed");
	CHECK(dev_reset(fd) == 0, "reset failed");
	CHECK(get_stats(fd, &s) == 0, "stats failed");
	CHECK(s.counter == 0 && s.buf_len == 0, "state not cleared: counter=%llu len=%u",
	      (unsigned long long)s.counter, s.buf_len);
	CHECK(lseek(fd, 0, SEEK_SET) == 0, "lseek failed");
	CHECK(read(fd, &c, 1) == 0, "buffer should read as empty after reset");
	close(fd);
	return 0;
}

static int t_unknown_ioctl_is_enotty(void)
{
	int fd = dev_open();

	CHECK(fd >= 0, "open failed");
	errno = 0;
	CHECK(ioctl(fd, _IO(VALDEV_IOC_MAGIC, 99)) == -1 && errno == ENOTTY, "wrong errno for unknown ioctl: %s",
	      strerror(errno));
	errno = 0;
	CHECK(ioctl(fd, 0xdeadbeef) == -1 && errno == ENOTTY, "wrong errno for garbage ioctl: %s", strerror(errno));
	close(fd);
	return 0;
}

static int t_bad_user_pointers_are_efault(void)
{
	char c;
	int fd = dev_open();

	CHECK(fd >= 0, "open failed");
	CHECK(write(fd, "data", 4) == 4, "write failed");
	CHECK(lseek(fd, 0, SEEK_SET) == 0, "lseek failed");

	errno = 0;
	CHECK(ioctl(fd, VALDEV_IOC_GET_VERSION, bad_ptr()) == -1 && errno == EFAULT, "GET_VERSION: %s", strerror(errno));
	errno = 0;
	CHECK(ioctl(fd, VALDEV_IOC_ADD_COUNTER, bad_ptr()) == -1 && errno == EFAULT, "ADD_COUNTER: %s", strerror(errno));
	errno = 0;
	CHECK(ioctl(fd, VALDEV_IOC_GET_STATS, bad_ptr()) == -1 && errno == EFAULT, "GET_STATS: %s", strerror(errno));
	errno = 0;
	CHECK(read(fd, bad_ptr(), 4) == -1 && errno == EFAULT, "read: %s", strerror(errno));
	errno = 0;
	CHECK(write(fd, bad_ptr(), 4) == -1 && errno == EFAULT, "write: %s", strerror(errno));

	/* the device must still be healthy afterwards */
	CHECK(read(fd, &c, 1) == 1 && c == 'd', "device state corrupted by a faulting call");
	close(fd);
	return 0;
}

static int t_stats_track_opens(void)
{
	struct valdev_stats before, after;
	int a = dev_open();
	int b;

	CHECK(a >= 0, "open failed");
	CHECK(get_stats(a, &before) == 0, "stats failed");
	b = dev_open();
	CHECK(b >= 0, "second open failed");
	CHECK(get_stats(a, &after) == 0, "stats failed");
	CHECK(after.opens == before.opens + 1, "opens went %llu -> %llu, expected +1",
	      (unsigned long long)before.opens, (unsigned long long)after.opens);
	CHECK(after.reserved == 0, "reserved field must be zero, got %u", after.reserved);
	close(a);
	close(b);
	return 0;
}

/* ------------------------------------------------------- concurrency tests */

#define THREADS 8

/* Race detection is probabilistic; raise VALDEV_ITERS for a harder soak. */
static long iters(void)
{
	const char *e = getenv("VALDEV_ITERS");
	long v = e ? strtol(e, NULL, 10) : 0;

	return v > 0 ? v : 5000;
}

static void *counter_worker(void *arg)
{
	int fd = dev_open();   /* each thread has its own open file */
	const long n = iters();
	long rc = 0;

	(void)arg;
	if (fd < 0)
		return (void *)1;
	for (long i = 0; i < n; i++) {
		uint64_t one = 1;

		if (ioctl(fd, VALDEV_IOC_ADD_COUNTER, &one) != 0) {
			rc = 1;
			break;
		}
	}
	close(fd);
	return (void *)rc;
}

static int t_concurrent_counter_is_atomic(void)
{
	pthread_t th[THREADS];
	struct valdev_stats s;
	int fd = dev_open();

	CHECK(fd >= 0, "open failed");
	for (int i = 0; i < THREADS; i++)
		CHECK(pthread_create(&th[i], NULL, counter_worker, NULL) == 0, "pthread_create failed");
	for (int i = 0; i < THREADS; i++) {
		void *rc;

		pthread_join(th[i], &rc);
		CHECK(rc == NULL, "worker %d failed", i);
	}
	CHECK(get_stats(fd, &s) == 0, "stats failed");
	CHECK(s.counter == (uint64_t)THREADS * (uint64_t)iters(), "lost updates: expected %llu, got %llu",
	      (unsigned long long)THREADS * (unsigned long long)iters(), (unsigned long long)s.counter);
	close(fd);
	return 0;
}

#define CHUNK 256

static void *writer_worker(void *arg)
{
	long id = (long)arg;
	char buf[CHUNK];
	int fd = dev_open();
	long rc = 0;

	if (fd < 0)
		return (void *)1;
	memset(buf, 'A' + (int)id, sizeof(buf));
	for (int i = 0; i < 200 && !rc; i++)
		if (pwrite(fd, buf, sizeof(buf), id * CHUNK) != CHUNK)
			rc = 1;
	close(fd);
	return (void *)rc;
}

static int t_concurrent_writers_do_not_corrupt(void)
{
	pthread_t th[4];
	char buf[CHUNK];
	int fd = dev_open();

	CHECK(fd >= 0, "open failed");
	for (long i = 0; i < 4; i++)
		CHECK(pthread_create(&th[i], NULL, writer_worker, (void *)i) == 0, "pthread_create failed");
	for (int i = 0; i < 4; i++) {
		void *rc;

		pthread_join(th[i], &rc);
		CHECK(rc == NULL, "writer %d failed", i);
	}
	for (int i = 0; i < 4; i++) {
		char expect[CHUNK];

		memset(expect, 'A' + i, sizeof(expect));
		CHECK(pread(fd, buf, sizeof(buf), i * CHUNK) == CHUNK, "readback %d short", i);
		CHECK(memcmp(buf, expect, CHUNK) == 0, "chunk %d corrupted", i);
	}
	close(fd);
	return 0;
}

/* ------------------------------------------------------------------- main */

struct test {
	const char *name;
	int (*fn)(void);
};

static const struct test tests[] = {
	{ "open_close",                        t_open_close },
	{ "version_matches_abi",               t_version_matches_abi },
	{ "write_read_roundtrip",              t_write_read_roundtrip },
	{ "read_empty_is_eof",                 t_read_empty_is_eof },
	{ "partial_reads_follow_offset",       t_partial_reads_follow_offset },
	{ "write_straddling_end_is_short",     t_write_straddling_end_is_short },
	{ "write_at_capacity_is_enospc",       t_write_at_capacity_is_enospc },
	{ "llseek_validates_offsets",          t_llseek_validates_offsets },
	{ "counter_accumulates",               t_counter_accumulates },
	{ "reset_clears_state",                t_reset_clears_state },
	{ "unknown_ioctl_is_enotty",           t_unknown_ioctl_is_enotty },
	{ "bad_user_pointers_are_efault",      t_bad_user_pointers_are_efault },
	{ "stats_track_opens",                 t_stats_track_opens },
	{ "concurrent_counter_is_atomic",      t_concurrent_counter_is_atomic },
	{ "concurrent_writers_do_not_corrupt", t_concurrent_writers_do_not_corrupt },
};

int main(int argc, char **argv)
{
	const int n = (int)(sizeof(tests) / sizeof(tests[0]));
	int failures = 0;

	if (argc > 1)
		dev_path = argv[1];

	int probe = dev_open();

	if (probe < 0) {
		printf("Bail out! cannot open %s: %s (is the module loaded? try: sudo insmod valdev.ko)\n",
		       dev_path, strerror(errno));
		return 2;
	}

	printf("TAP version 13\n1..%d\n", n);
	for (int i = 0; i < n; i++) {
		why[0] = '\0';
		dev_reset(probe);   /* every test starts from a clean device */
		if (tests[i].fn() == 0) {
			printf("ok %d - %s\n", i + 1, tests[i].name);
		} else {
			printf("not ok %d - %s # %s\n", i + 1, tests[i].name, why);
			failures++;
		}
		fflush(stdout);
	}
	close(probe);

	printf("# %d passed, %d failed\n", n - failures, failures);
	return failures ? 1 : 0;
}
