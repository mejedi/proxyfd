#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/anon_inodes.h>
#include <asm/ioctls.h>
#define DEVICE_NAME "proxyfd"
#define CLASS_NAME  "proxyfd"

static int major;
static struct class *class;

struct request {
	__u32 flags;    /* O_CLOEXEC, O_NONBLOCK */
	__u32 wrmax;    /* single write limit (0 - disable) */
	__u32 fd_io;    /* file to use for io */
	__u32 fd_ioctl; /* file to use for ioctl */
};

struct proxy_ctx {
	struct file *filp_io;
	struct file *filp_ioctl;
	__u32        wrmax;
};

ssize_t proxy_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct file *filep    = iocb->ki_filp;
	struct proxy_ctx *ctx = filep->private_data;

	return vfs_iter_read(ctx->filp_io, to, &iocb->ki_pos, 0);
}

ssize_t proxy_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *filep    = iocb->ki_filp;
	struct proxy_ctx *ctx = filep->private_data;

	if (ctx->wrmax)
		iov_iter_truncate(from, ctx->wrmax);

	return vfs_iter_write(ctx->filp_io, from, &iocb->ki_pos, 0);
}

static struct file *ioctl_route(struct proxy_ctx *ctx, unsigned cmd)
{
	switch (cmd) {
	case FIONREAD:
		return ctx->filp_io;
	default:
		return ctx->filp_ioctl;
	}
}

static long proxy_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct proxy_ctx *ctx = filp->private_data;
	struct file *fwd;

	fwd = ioctl_route(ctx, cmd);

	if (fwd->f_op->unlocked_ioctl) {
		return fwd->f_op->unlocked_ioctl(fwd, cmd, arg);
	}

	return -ENOTTY;
}

static long proxy_compat_ioctl(struct file *filp,
                               unsigned int cmd, unsigned long arg)
{
	struct proxy_ctx *ctx = filp->private_data;
	struct file *fwd;

	fwd = ioctl_route(ctx, cmd);

	if (fwd->f_op->compat_ioctl) {
		return fwd->f_op->compat_ioctl(fwd, cmd, arg);
	}

	return -ENOTTY;
}

static int proxy_close(struct inode *inode, struct file *filp)
{
	struct proxy_ctx *ctx = filp->private_data;

	fput(ctx->filp_io);
	fput(ctx->filp_ioctl);

	return 0;
}

static struct file_operations proxy_fops = {
	.owner = THIS_MODULE,

	.llseek	        = no_llseek,
	.read_iter      = proxy_read_iter,
	.write_iter     = proxy_write_iter,
	.unlocked_ioctl = proxy_ioctl,
	.compat_ioctl   = proxy_compat_ioctl,
	.release        = proxy_close,
	/* TODO .poll .fasync */
};

static int dev_open(struct inode *inode, struct file *filp)
{
	if (iminor(inode))
		return -ENXIO;
	return 0;
}

static ssize_t dev_write(struct file *filp, const char __user *buf,
                         size_t count, loff_t *ppos)
{
	ssize_t rc;
	struct file *filp_io, *filp_ioctl;
	struct proxy_ctx *ctx;
	struct request r;

	if (count != sizeof(r))
		return -EINVAL;

	if (copy_from_user(&r, buf, sizeof(r)))
		return -EFAULT;

	if (r.flags & ~(__u32)(O_CLOEXEC | O_NONBLOCK))
		return -EINVAL;

	filp_io = fget(r.fd_io);
	if (!filp_io)
		return -EBADF;

	if (filp_io->f_op == &proxy_fops) {
		/* Otherwize, arbitrary large trees would be possible. */
		rc = -EPERM;
		goto error_fput_filp_io;
	}

	if (!filp_io->f_op->write_iter || !filp_io->f_op->read_iter) {
		rc = -EINVAL;
		goto error_fput_filp_io;
	}

	filp_ioctl = fget(r.fd_ioctl);
	if (!filp_ioctl) {
		rc = -EBADF;
		goto error_fput_filp_io;
	}

	if (filp_ioctl->f_op == &proxy_fops) {
		/* Otherwize, arbitrary large trees would be possible. */
		rc = -EPERM;
		goto error_fput_filp_ioctl;
	}

	ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		rc = -ENOMEM;
		goto error_fput_filp_ioctl;
	}

	ctx->filp_io = filp_io;
	ctx->filp_ioctl = filp_ioctl;
	ctx->wrmax = r.wrmax;

	rc = anon_inode_getfd("[proxyfd]", &proxy_fops, ctx,
	                      O_RDWR | (r.flags & (O_CLOEXEC | O_NONBLOCK)));
	if (rc < 0) {
		kfree(ctx);
		goto error_fput_filp_ioctl;
	}

	// FIXME GC not aware of filp_io and filp_ioctl links
	return rc;

error_fput_filp_ioctl:
	fput(filp_ioctl);
error_fput_filp_io:
	fput(filp_io);
	return rc;
}

static struct file_operations dev_fops =
{
	.owner = THIS_MODULE,

	.open  = dev_open,
	.write = dev_write,
};

static char *dev_node(struct device *dev, umode_t *mode)
{
	if (mode)
		*mode = 0222;

	return NULL;
}

static int __init mod_init(void)
{
	int rc;
	static struct device *device;

	major = register_chrdev(0, DEVICE_NAME, &dev_fops);
	if (major < 0)
		return  major;

	class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(class)) {
		rc = PTR_ERR(class);
		goto error_unregister_chrdev;
	}
	class->devnode = dev_node;

	device = device_create(class, NULL, MKDEV(major, 0), NULL,
	                       DEVICE_NAME);
	if (IS_ERR(device)) {
		rc = PTR_ERR(device);
		goto error_class_destroy;
	}

	return 0;

error_class_destroy:
	class_destroy(class);
error_unregister_chrdev:
	unregister_chrdev(major, DEVICE_NAME);
	return rc;
}

static void __exit mod_exit(void)
{
	device_destroy(class, MKDEV(major, 0));
	class_unregister(class);
	class_destroy(class);
	unregister_chrdev(major, DEVICE_NAME);
}

module_init(mod_init);
module_exit(mod_exit);

MODULE_LICENSE("GPL");
