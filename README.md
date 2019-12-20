# proxyfd

Linux kernel driver implementing proxy files. Proxy file sits in front 
of a pipe write end.  Every chunk of data written via proxy is prefixed
with the chunk length in atomic fashion.  The length is OR-combined with
a cookie set at creation time.  With cookies it is possible to tell the
origin if multiple proxies use the same pipe.

## Usage:

Device node: `/dev/proxyfd`.  To request a proxy, write the following
request into device:

```c
struct request
{
  uint32_t flags; // O_CLOEXEC
  uint32_t cookie;
  uint32_t pipefd;
};
```

`write()` result is either an error or a new (proxy) file descriptor.

Check `user.c` for usage example.

## Install:

```
make && sudo insmod proxyfd.ko
```
