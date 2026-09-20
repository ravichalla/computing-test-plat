// SPDX-License-Identifier: GPL-2.0
/*
 * valdev - a tiny character device used as a validation target.
 *
 * It exposes:
 *   - a 4 KiB byte buffer with read/write/llseek semantics
 *   - a 64-bit counter manipulated through ioctl()
 *   - simple statistics
 *
 * The point is not the device, it is having a real kernel/user boundary to
 * test: bounds handling, error codes (ENOSPC, ENOTTY, EFAULT), and locking
 * under concurrency. See test_valdev.c for the userspace test suite.
 *
 * Build:  make module      Load: sudo insmod valdev.ko
 * Run it inside a throwaway VM, never on a machine you care about.
 */

#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "valdev_ioctl.h"

static bool debug;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Log every operation (default: off)");

#define vd_dbg(fmt, ...) \
	do { if (debug) pr_info(VALDEV_NAME ": " fmt, ##__VA_ARGS__); } while (0)

struct valdev_state {
	struct mutex lock;                 /* protects everything below except opens */
	u8 buf[VALDEV_BUF_SIZE];
	u32 len;                           /* highest byte written + 1 */
	u64 counter;
	atomic64_t opens;
};

static struct valdev_state vd = {
	.opens = ATOMIC64_INIT(0),
};

static int valdev_open(struct inode *inode, struct file *filp)
{
	atomic64_inc(&vd.opens);
	vd_dbg("open (total %lld)\n", (long long)atomic64_read(&vd.opens));
	return 0;
}

static int valdev_release(struct inode *inode, struct file *filp)
{
	vd_dbg("release\n");
	return 0;
}

static ssize_t valdev_read(struct file *filp, char __user *ubuf, size_t count, loff_t *ppos)
{
	u8 *snap;
	ssize_t ret;

	if (count == 0)
		return 0;

	mutex_lock(&vd.lock);
	if (*ppos < 0 || *ppos >= vd.len) {
		mutex_unlock(&vd.lock);
		return 0;                       /* EOF */
	}
	count = min_t(size_t, count, vd.len - (size_t)*ppos);
	/*
	 * Snapshot under the lock, copy out after dropping it: a page fault in
	 * copy_to_user() must not stall other users of the device, and the reader
	 * gets a consistent view of the data.
	 */
	snap = kmemdup(vd.buf + *ppos, count, GFP_KERNEL);
	mutex_unlock(&vd.lock);
	if (!snap)
		return -ENOMEM;

	if (copy_to_user(ubuf, snap, count)) {
		ret = -EFAULT;
	} else {
		*ppos += count;
		ret = count;
	}
	kfree(snap);
	return ret;
}

static ssize_t valdev_write(struct file *filp, const char __user *ubuf, size_t count, loff_t *ppos)
{
	void *tmp;

	if (count == 0)
		return 0;
	if (*ppos < 0 || *ppos >= VALDEV_BUF_SIZE)
		return -ENOSPC;                 /* nothing fits at this offset */
	/* A write that straddles the end is truncated (short write), like a full disk. */
	count = min_t(size_t, count, VALDEV_BUF_SIZE - (size_t)*ppos);

	/*
	 * Copy from userspace into a bounce buffer first. copy_from_user() zero-fills
	 * the uncopied tail of its destination when it faults, so copying straight
	 * into the shared buffer would let one bad pointer destroy existing data.
	 * (t_bad_user_pointers_are_efault in test_valdev.c guards this.)
	 */
	tmp = memdup_user(ubuf, count);
	if (IS_ERR(tmp))
		return PTR_ERR(tmp);

	mutex_lock(&vd.lock);
	memcpy(vd.buf + *ppos, tmp, count);
	*ppos += count;
	if (*ppos > vd.len)
		vd.len = *ppos;
	mutex_unlock(&vd.lock);
	kfree(tmp);
	return count;
}

static long valdev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	void __user *uarg = (void __user *)arg;
	long ret = 0;

	switch (cmd) {
	case VALDEV_IOC_GET_VERSION: {
		u32 v = VALDEV_ABI_VERSION;

		if (copy_to_user(uarg, &v, sizeof(v)))
			ret = -EFAULT;
		break;
	}
	case VALDEV_IOC_ADD_COUNTER: {
		u64 delta;

		if (copy_from_user(&delta, uarg, sizeof(delta))) {
			ret = -EFAULT;
			break;
		}
		mutex_lock(&vd.lock);
		vd.counter += delta;            /* wraps on overflow by design */
		delta = vd.counter;
		mutex_unlock(&vd.lock);
		if (copy_to_user(uarg, &delta, sizeof(delta)))
			ret = -EFAULT;
		break;
	}
	case VALDEV_IOC_RESET:
		mutex_lock(&vd.lock);
		vd.counter = 0;
		vd.len = 0;
		memset(vd.buf, 0, sizeof(vd.buf));
		mutex_unlock(&vd.lock);
		break;
	case VALDEV_IOC_GET_STATS: {
		struct valdev_stats s = { 0 };

		mutex_lock(&vd.lock);
		s.counter = vd.counter;
		s.buf_len = vd.len;
		mutex_unlock(&vd.lock);
		s.opens = atomic64_read(&vd.opens);
		if (copy_to_user(uarg, &s, sizeof(s)))
			ret = -EFAULT;
		break;
	}
	default:
		ret = -ENOTTY;                  /* the correct errno for an unknown ioctl */
	}
	return ret;
}

static loff_t valdev_llseek(struct file *filp, loff_t off, int whence)
{
	loff_t base, newpos;

	switch (whence) {
	case SEEK_SET:
		base = 0;
		break;
	case SEEK_CUR:
		base = filp->f_pos;
		break;
	case SEEK_END:
		mutex_lock(&vd.lock);
		base = vd.len;
		mutex_unlock(&vd.lock);
		break;
	default:
		return -EINVAL;
	}
	if (check_add_overflow(base, off, &newpos))
		return -EINVAL;
	/* Seeking to exactly VALDEV_BUF_SIZE is allowed (a write there gets ENOSPC). */
	return vfs_setpos(filp, newpos, VALDEV_BUF_SIZE);
}

static const struct file_operations valdev_fops = {
	.owner          = THIS_MODULE,
	.open           = valdev_open,
	.release        = valdev_release,
	.read           = valdev_read,
	.write          = valdev_write,
	.llseek         = valdev_llseek,
	.unlocked_ioctl = valdev_ioctl,
};

static struct miscdevice valdev_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = VALDEV_NAME,
	.fops  = &valdev_fops,
	.mode  = 0666,                          /* test device: let unprivileged tests open it */
};

static int __init valdev_init(void)
{
	int ret;

	mutex_init(&vd.lock);
	ret = misc_register(&valdev_misc);
	if (ret) {
		pr_err(VALDEV_NAME ": misc_register failed (%d)\n", ret);
		return ret;
	}
	pr_info(VALDEV_NAME ": loaded, ABI %u.%u, /dev/%s\n", VALDEV_ABI_MAJOR, VALDEV_ABI_MINOR, VALDEV_NAME);
	return 0;
}

static void __exit valdev_exit(void)
{
	misc_deregister(&valdev_misc);
	pr_info(VALDEV_NAME ": unloaded\n");
}

module_init(valdev_init);
module_exit(valdev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name <you@example.com>");
MODULE_DESCRIPTION("Validation target: buffer + counter char device");
MODULE_VERSION("1.0");
