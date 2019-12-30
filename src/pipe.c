/* proxyfd kernel module
 *
 * pipe_framed_write - inserts a header before each chunk submitted;
 *                     header is a BE __u32 chunk len, OR-d with a cookie.
 *
 * Based on pipe_write from linux/fs/pipe.c.  As crazy as it sounds, we
 * have a lot of code copied verbatim.  Should work, though.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/uio.h>
#include <linux/pipe_fs_i.h>
#include <linux/sched/signal.h>
#include <linux/poll.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/eventpoll.h>

/* Taken verbatim from linux/fs/pipe.c. */
static void anon_pipe_buf_release(struct pipe_inode_info *pipe,
				  struct pipe_buffer *buf)
{
	struct page *page = buf->page;

	if (page_count(page) == 1 && !pipe->tmp_page)
		pipe->tmp_page = page;
	else
		put_page(page);
}

/* Stealing is prevented. Could've taken linux/fs/pipe.c code but then
 * even more unexported stuff would pop up. */
static int anon_pipe_buf_steal(struct pipe_inode_info *pipe,
			       struct pipe_buffer *buf)
{
	return 1;
}

/* Taken verbatim from linux/fs/pipe.c.
 * Technically, this is distinct from genuine anon_pipe_buf_ops.
 * Luckily, pipe is prep-d to have weird buffers.
 * Consequences:
 *
 *   * normal pipe writes and framed pipe writes never go to the same
 *     buffer, adjacent small framed writes are coalesced as they should;
 *
 *   * framed writes ignore O_DIRECT (packet mode), normal writes
 *     behave as expected, even if interleaved with framed ones;
 *
 *   * stealing is prevented for buffers spawned by framed writes;
 *     no splicing today.
 */
static const struct pipe_buf_operations anon_pipe_buf_ops =
{
	.confirm = generic_pipe_buf_confirm,
	.release = anon_pipe_buf_release,
	.steal = anon_pipe_buf_steal,
	.get = generic_pipe_buf_get,
};

/* Taken verbatim from linux/fs/pipe.c. */
static bool pipe_buf_can_merge(struct pipe_buffer *buf)
{
    return buf->ops == &anon_pipe_buf_ops;
}

/* Seems identical. */
#define __pipe_lock   pipe_lock
#define __pipe_unlock pipe_unlock

#define HDR 4

/* pipe_write from linux/fs/pipe.c with minor changes. */
ssize_t
pipe_framed_write(struct file *filp, struct iov_iter *from, __u32 cookie)
{
	struct pipe_inode_info *pipe = filp->private_data;
	ssize_t ret = 0;
	int do_wakeup = 0;
	size_t total_len = iov_iter_count(from);
	size_t overhead;
	ssize_t chars;
	char *kaddr;
	__u32 hdr;

	/* Null write succeeds. */
	if (unlikely(total_len == 0))
		return 0;

	__pipe_lock(pipe);

	if (!pipe->readers) {
		send_sig(SIGPIPE, current, 0);
		ret = -EPIPE;
		goto out;
	}

	/* HDR for incomplete page not accounted for in overhead */
	overhead = (total_len / PAGE_SIZE) * HDR;

	/* We try to merge small writes */
	chars = (total_len + overhead) & (PAGE_SIZE-1); /* size of the last buffer */
	if (pipe->nrbufs && chars != 0) {
		int lastbuf = (pipe->curbuf + pipe->nrbufs - 1) &
							(pipe->buffers - 1);
		struct pipe_buffer *buf = pipe->bufs + lastbuf;
		int offset = buf->offset + buf->len;

		if (pipe_buf_can_merge(buf) && offset + HDR + chars <= PAGE_SIZE) {
			ret = pipe_buf_confirm(pipe, buf);
			if (ret)
				goto out;
			kaddr = kmap_atomic(buf->page);
			hdr = cookie | htonl((__u32)chars);
			memcpy(kaddr + offset, &hdr, 4);
			ret = copy_from_iter(kaddr + offset + HDR, chars, from);
			kunmap_atomic(kaddr);
			if (unlikely(ret < chars)) {
				ret = -EFAULT;
				goto out;
			}

			do_wakeup = 1;
			buf->len += HDR + ret;
			if (!iov_iter_count(from))
				goto out;
		}
	}

	for (;;) {
		int bufs;

		if (!pipe->readers) {
			send_sig(SIGPIPE, current, 0);
			if (!ret)
				ret = -EPIPE;
			break;
		}
		bufs = pipe->nrbufs;
		if (bufs < pipe->buffers) {
			int newbuf = (pipe->curbuf + bufs) & (pipe->buffers-1);
			struct pipe_buffer *buf = pipe->bufs + newbuf;
			struct page *page = pipe->tmp_page;
			int copied;

			if (!page) {
				page = alloc_page(GFP_HIGHUSER | __GFP_ACCOUNT);
				if (unlikely(!page)) {
					ret = ret ? : -ENOMEM;
					break;
				}
				pipe->tmp_page = page;
			}
			/* Always wake up, even if the copy fails. Otherwise
			 * we lock up (O_NONBLOCK-)readers that sleep due to
			 * syscall merging.
			 * FIXME! Is this really true?
			 */
			do_wakeup = 1;
			kaddr = kmap_atomic(page);
			copied = copy_from_iter(kaddr + HDR, PAGE_SIZE - HDR, from);
			if (unlikely(copied < PAGE_SIZE - HDR && iov_iter_count(from))) {
				kunmap_atomic(kaddr);
				if (!ret)
					ret = -EFAULT;
				break;
			}
			hdr = cookie | htonl((__u32)copied);
			memcpy(kaddr, &hdr, HDR);
			kunmap_atomic(kaddr);

			ret += copied;

			/* Insert it into the buffer array */
			buf->page = page;
			buf->ops = &anon_pipe_buf_ops;
			buf->offset = 0;
			buf->len = copied + HDR;
			buf->flags = 0;
			pipe->nrbufs = ++bufs;
			pipe->tmp_page = NULL;

			if (!iov_iter_count(from))
				break;
		}
		if (bufs < pipe->buffers)
			continue;
		if (filp->f_flags & O_NONBLOCK) {
			if (!ret)
				ret = -EAGAIN;
			break;
		}
		if (signal_pending(current)) {
			if (!ret)
				ret = -ERESTARTSYS;
			break;
		}
		if (do_wakeup) {
			wake_up_interruptible_sync_poll(&pipe->wait, EPOLLIN | EPOLLRDNORM);
			kill_fasync(&pipe->fasync_readers, SIGIO, POLL_IN);
			do_wakeup = 0;
		}
		pipe->waiting_writers++;
		pipe_wait(pipe);
		pipe->waiting_writers--;
	}

out:
	__pipe_unlock(pipe);
	if (do_wakeup) {
		wake_up_interruptible_sync_poll(&pipe->wait, EPOLLIN | EPOLLRDNORM);
		kill_fasync(&pipe->fasync_readers, SIGIO, POLL_IN);
	}
	if (ret > 0 && sb_start_write_trylock(file_inode(filp)->i_sb)) {
		int err = file_update_time(filp);
		if (err)
			ret = err;
		sb_end_write(file_inode(filp)->i_sb);
	}
	return ret;
}

/* Taken verbatim from linux/fs/pipe.c. */
void pipe_wait(struct pipe_inode_info *pipe)
{
	DEFINE_WAIT(wait);

	/*
	 * Pipes are system-local resources, so sleeping on them
	 * is considered a noninteractive wait:
	 */
	prepare_to_wait(&pipe->wait, &wait, TASK_INTERRUPTIBLE);
	pipe_unlock(pipe);
	schedule();
	finish_wait(&pipe->wait, &wait);
	pipe_lock(pipe);
}
