#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/slab.h>
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

struct proxy_req {
	__u32 flags;    /* O_CLOEXEC, O_NONBLOCK */
	__u32 cookie;
	__u32 pipefd;
};

struct proxy_ctx {
	struct file *pipe;
	__u32        cookie;
};

ssize_t proxy_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	return -EBADF;
}

ssize_t pipe_framed_write(struct kiocb *iocb, struct iov_iter *from,
                          __u32 cookie);

ssize_t proxy_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *filep    = iocb->ki_filp;
	struct proxy_ctx *ctx = filep->private_data;

	iocb->ki_filp = ctx->pipe;
	return pipe_framed_write(iocb, from, ctx->cookie);
}

static long proxy_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct proxy_ctx *ctx = filp->private_data;

	if (cmd == TCGETS)
		return 0;

	if (ctx->pipe->f_op->unlocked_ioctl) {
		return ctx->pipe->f_op->unlocked_ioctl(ctx->pipe, cmd, arg);
	}

	return -ENOTTY;
}

static long proxy_compat_ioctl(struct file *filp,
                               unsigned int cmd, unsigned long arg)
{
	struct proxy_ctx *ctx = filp->private_data;

	if (cmd == TCGETS)
		return 0;

	if (ctx->pipe->f_op->compat_ioctl) {
		return ctx->pipe->f_op->compat_ioctl(ctx->pipe, cmd, arg);
	}

	return -ENOTTY;
}

static int proxy_close(struct inode *inode, struct file *filp)
{
	struct proxy_ctx *ctx = filp->private_data;

	fput(ctx->pipe);

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
	struct file *pipe;
	struct proxy_ctx *ctx;
	struct proxy_req r;

	if (count != sizeof(r))
		return -EINVAL;

	if (copy_from_user(&r, buf, sizeof(r)))
		return -EFAULT;

	if (r.flags & ~(__u32)(O_CLOEXEC | O_NONBLOCK))
		return -EINVAL;

	pipe = fget(r.pipefd);
	if (!pipe)
		return -EBADF;

	/* TODO ensure this is a pipe */
	if (strcmp(pipe->f_inode->i_sb->s_type->name, "pipefs")) {
		rc = -EINVAL;
		goto error_fput_pipe;
	}

	ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		rc = -ENOMEM;
		goto error_fput_pipe;
	}

	ctx->pipe = pipe;
	ctx->cookie = r.cookie;

	rc = anon_inode_getfd("[proxyfd]", &proxy_fops, ctx,
	                      O_RDWR | (r.flags & (O_CLOEXEC | O_NONBLOCK)));
	if (rc < 0) {
		kfree(ctx);
		goto error_fput_pipe;
	}

	return rc;

error_fput_pipe:
	fput(pipe);
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
