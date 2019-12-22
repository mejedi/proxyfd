#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/mount.h>
#include <asm/ioctls.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,3,0)
#include <linux/pseudo_fs.h>
#endif

#define DEVICE_NAME "proxyfd"
#define CLASS_NAME  "proxyfd"

static int major;
static struct class *class;
static struct vfsmount *proxy_inode_mnt __read_mostly;
static struct inode *proxy_inode_inode;

struct proxy_req {
	__u32 flags; /* O_CLOEXEC, O_NONBLOCK */
	__u32 cookie;
	__u32 pipefd;
};

struct proxy_ctx {
	struct file *pipe;
	__u32        cookie;
};

/* proxy file methods */

ssize_t pipe_framed_write(struct file *filep, struct iov_iter *from,
                          __u32 cookie);

ssize_t proxy_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct proxy_ctx *ctx = iocb->ki_filp->private_data;

	return pipe_framed_write(ctx->pipe, from, ctx->cookie);
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
	.write_iter     = proxy_write_iter,
	.unlocked_ioctl = proxy_ioctl,
	.compat_ioctl   = proxy_compat_ioctl,
	.release        = proxy_close,
};

/* control device */

static int proxy_getfd(void *ctx, int flags)
{
	int rc;
	struct file *file;
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,3,0)
	struct qstr this;
	struct path path;

	this.name = "proxy";
	this.len = 5;
	this.hash = 0;
	path.dentry = d_alloc_pseudo(proxy_inode_mnt->mnt_sb, &this);
	if (!path.dentry)
		return -ENOMEM;
	path.mnt = mntget(proxy_inode_mnt);

	ihold(proxy_inode_inode);
	d_instantiate(path.dentry, proxy_inode_inode);

	file = alloc_file(&path, OPEN_FMODE(flags), &proxy_fops);
	if (IS_ERR(file)) {
		rc = PTR_ERR(file);
		goto err_dput;
	}

	file->f_flags = flags & (O_ACCMODE | O_NONBLOCK);
#else
	ihold(proxy_inode_inode);
	file = alloc_file_pseudo(proxy_inode_inode, proxy_inode_mnt, "proxy",
				 flags & (O_ACCMODE | O_NONBLOCK), &proxy_fops);
#endif

	file->f_mapping = proxy_inode_inode->i_mapping;
	file->private_data = ctx;

	__module_get(THIS_MODULE);

	rc = get_unused_fd_flags(flags);
	if (rc < 0)
		goto err_fput;

	fd_install(rc, file);
	return rc;

err_fput:
	fput(file);
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,3,0)
err_dput:
	path_put(&path);
#endif
	return rc;
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

	if (!(pipe->f_mode & FMODE_WRITE)) {
		rc = -EBADF;
		goto error_fput_pipe;
	}

	/* pipefifo_fops unexported */
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

	rc = proxy_getfd(ctx,
	                 O_WRONLY | (r.flags & (O_CLOEXEC | O_NONBLOCK)));
	if (rc < 0) {
		kfree(ctx);
		goto error_fput_pipe;
	}

	return rc;

error_fput_pipe:
	fput(pipe);
	return rc;
}

static int dev_open(struct inode *inode, struct file *filp)
{
	if (iminor(inode))
		return -ENXIO;
	return 0;
}

static struct file_operations dev_fops =
{
	.owner = THIS_MODULE,

	.open  = dev_open,
	.write = dev_write,
};

/* pseudo filesystem */

static char *proxy_inodefs_dname(struct dentry *dentry, char *buffer, int buflen)
{
#define DNAME		"[proxyfd]"
#define DNAME_LEN	sizeof(DNAME)
	if (buflen < DNAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);
	return memcpy(buffer + buflen - DNAME_LEN, DNAME, DNAME_LEN);
}

static const struct dentry_operations proxy_inodefs_dentry_operations = {
	.d_dname	= proxy_inodefs_dname,
};

#if LINUX_VERSION_CODE < KERNEL_VERSION(5,3,0)
static struct dentry *proxy_inodefs_mount(struct file_system_type *fs_type,
				int flags, const char *dev_name, void *data)
{
	return mount_pseudo(fs_type, "proxy_inode:", NULL,
			&proxy_inodefs_dentry_operations, 0);
}
#else
static int proxy_inodefs_init_fs_context(struct fs_context *fc)
{
	struct pseudo_fs_context *ctx = init_pseudo(fc, 0);
	if (!ctx)
		return -ENOMEM;
	ctx->dops = &proxy_inodefs_dentry_operations;
	return 0;
}
#endif

static struct file_system_type proxy_inode_fs_type = {
	.name		= "proxy_inodefs",
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,3,0)
	.mount		= proxy_inodefs_mount,
#else
	.init_fs_context = proxy_inodefs_init_fs_context,
#endif
	.kill_sb	= kill_anon_super,
};

/* module lifetime */

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

	proxy_inode_mnt = kern_mount(&proxy_inode_fs_type);
	if (IS_ERR(proxy_inode_mnt))
		return PTR_ERR(proxy_inode_mnt);

	proxy_inode_inode = alloc_anon_inode(proxy_inode_mnt->mnt_sb);
	if (IS_ERR(proxy_inode_inode)) {
		rc = PTR_ERR(proxy_inode_inode);
		goto error_iput;
	}
	proxy_inode_inode->i_mode = S_IFCHR | S_IWUSR;
	/* we need it recognized as a character device */

	major = register_chrdev(0, DEVICE_NAME, &dev_fops);
	if (major < 0) {
		rc = major;
		goto error_unmount;
	}

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
error_iput:
	iput(proxy_inode_inode);
error_unmount:
	kern_unmount(proxy_inode_mnt);
	return rc;
}

static void __exit mod_exit(void)
{
	device_destroy(class, MKDEV(major, 0));
	class_unregister(class);
	class_destroy(class);
	unregister_chrdev(major, DEVICE_NAME);
	iput(proxy_inode_inode);
	kern_unmount(proxy_inode_mnt);
}

module_init(mod_init);
module_exit(mod_exit);

MODULE_LICENSE("GPL");
