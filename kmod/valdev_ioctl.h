/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * valdev - user/kernel ABI.
 *
 * Shared by the kernel module and the userspace test program, so both sides
 * always agree on the ioctl numbers and struct layouts.
 */
#ifndef VALDEV_IOCTL_H
#define VALDEV_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define VALDEV_NAME        "valdev"
#define VALDEV_BUF_SIZE    4096u

/* Version is (major << 16) | minor. Bump major on any ABI break. */
#define VALDEV_ABI_MAJOR   1u
#define VALDEV_ABI_MINOR   0u
#define VALDEV_ABI_VERSION ((VALDEV_ABI_MAJOR << 16) | VALDEV_ABI_MINOR)

struct valdev_stats {
	__u64 counter;   /* current counter value */
	__u64 opens;     /* number of open() calls since module load */
	__u32 buf_len;   /* bytes currently valid in the data buffer */
	__u32 reserved;  /* must be zero; keeps the struct 8-byte aligned */
};

#define VALDEV_IOC_MAGIC 'V'

/* Read the ABI version. */
#define VALDEV_IOC_GET_VERSION _IOR(VALDEV_IOC_MAGIC, 1, __u32)
/* Add the given delta to the counter; the new value is written back. */
#define VALDEV_IOC_ADD_COUNTER _IOWR(VALDEV_IOC_MAGIC, 2, __u64)
/* Clear the counter and the data buffer. */
#define VALDEV_IOC_RESET       _IO(VALDEV_IOC_MAGIC, 3)
/* Snapshot counter, open count and buffer length atomically. */
#define VALDEV_IOC_GET_STATS   _IOR(VALDEV_IOC_MAGIC, 4, struct valdev_stats)

#endif /* VALDEV_IOCTL_H */
