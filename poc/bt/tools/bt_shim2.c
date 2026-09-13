/* IPC trace shim v2: log socket ops on ALL fds (skip own log fd) */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int logfd = -1;
static int in_lg = 0;
static int (*ropen)(const char*, int, ...);

static void lg(const char *fmt, ...)
{
    if (in_lg) return;
    in_lg = 1;
    if (logfd < 0) {
        if (!ropen) ropen = dlsym(RTLD_NEXT, "open");
        if (ropen) logfd = ropen("/tmp/shim.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    }
    if (logfd >= 0) {
        char buf[512];
        va_list ap; va_start(ap, fmt);
        int n = vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        if (n > 0) write(logfd, buf, n);
    }
    in_lg = 0;
}

__attribute__((constructor)) static void shim_init(void)
{
    lg("SHIM v2 loaded pid=%d\n", getpid());
}

static void hx(const void *p, int k, char *out)
{
    const unsigned char *b = p;
    for (int i = 0; i < k; i++) out += sprintf(out, "%02x", b[i]);
    *out = 0;
}

int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    static int (*real)(int, const struct sockaddr*, socklen_t);
    if (!real) real = dlsym(RTLD_NEXT, "connect");
    int r = real(fd, addr, len);
    if (addr && addr->sa_family == AF_UNIX) {
        const struct sockaddr_un *u = (const void*)addr;
        char hp[64] = "";
        const unsigned char *q = (const unsigned char*)u->sun_path;
        for (int i = 0; i < 24 && i < (int)(len - 2); i++) sprintf(hp + 2*i, "%02x", q[i]);
        lg("connect(%d, len=%u, hex=%s str=%s) = %d\n", fd, len, hp, u->sun_path, r);
    } else if (addr) {
        lg("connect(%d, family=%d) = %d\n", fd, addr->sa_family, r);
    }
    return r;
}

ssize_t send(int fd, const void *buf, size_t n, int flags)
{
    static ssize_t (*real)(int, const void*, size_t, int);
    if (!real) real = dlsym(RTLD_NEXT, "send");
    ssize_t r = real(fd, buf, n, flags);
    if (fd != logfd && n >= 2) {
        char h[600]; hx(buf, n < 256 ? (int)n : 256, h);
        lg("send(%d,%zu) %s -> %zd\n", fd, n, h, r);
    }
    return r;
}

ssize_t sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *a, socklen_t l)
{
    static ssize_t (*real)(int, const void*, size_t, int, const struct sockaddr*, socklen_t);
    if (!real) real = dlsym(RTLD_NEXT, "sendto");
    ssize_t r = real(fd, buf, n, flags, a, l);
    if (fd != logfd) {
        char h[600]; hx(buf, n < 256 ? (int)n : 256, h);
        lg("sendto(%d,%zu) %s -> %zd\n", fd, n, h, r);
    }
    return r;
}

ssize_t write(int fd, const void *buf, size_t n)
{
    static ssize_t (*real)(int, const void*, size_t);
    if (!real) real = dlsym(RTLD_NEXT, "write");
    ssize_t r = real(fd, buf, n);
    if (fd != logfd && n >= 2) {
        char h[600]; hx(buf, n < 256 ? (int)n : 256, h);
        lg("write(%d,%zu) %s -> %zd\n", fd, n, h, r);
    }
    return r;
}

static void cm(struct msghdr *m)
{
    for (struct cmsghdr *c = CMSG_FIRSTHDR(m); c; c = CMSG_NXTHDR(m, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int nf = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            lg("  SCM_RIGHTS n=%d:", nf);
            int *f = (int*)CMSG_DATA(c);
            for (int i = 0; i < nf; i++) lg(" fd=%d", f[i]);
            lg("\n");
        }
    }
}

ssize_t sendmsg(int fd, const struct msghdr *m, int flags)
{
    static ssize_t (*real)(int, const struct msghdr*, int);
    if (!real) real = dlsym(RTLD_NEXT, "sendmsg");
    ssize_t r = real(fd, m, flags);
    if (fd != logfd && m && m->msg_iovlen > 0) {
        char h[600]; hx(m->msg_iov[0].iov_base, 8, h);
        lg("sendmsg(%d) %s -> %zd\n", fd, h, r);
        cm((struct msghdr*)m);
    }
    return r;
}

ssize_t recvmsg(int fd, struct msghdr *m, int flags)
{
    static ssize_t (*real)(int, struct msghdr*, int);
    if (!real) real = dlsym(RTLD_NEXT, "recvmsg");
    ssize_t r = real(fd, m, flags);
    if (fd != logfd && r > 0) {
        char h[600] = "";
        if (m->msg_iovlen > 0) hx(m->msg_iov[0].iov_base, 8, h);
        lg("recvmsg(%d) %zd %s\n", fd, r, h);
        cm(m);
    }
    return r;
}

ssize_t recv(int fd, void *buf, size_t n, int flags)
{
    static ssize_t (*real)(int, void*, size_t, int);
    if (!real) real = dlsym(RTLD_NEXT, "recv");
    ssize_t r = real(fd, buf, n, flags);
    if (fd != logfd && r > 0) {
        char h[600]; hx(buf, r < 8 ? (int)r : 8, h);
        lg("recv(%d,%zu) %zd %s\n", fd, n, r, h);
    }
    return r;
}

ssize_t read(int fd, void *buf, size_t n)
{
    static ssize_t (*real)(int, void*, size_t);
    if (!real) real = dlsym(RTLD_NEXT, "read");
    ssize_t r = real(fd, buf, n);
    if (fd != logfd && r > 0) {
        char h[600]; hx(buf, r < 8 ? (int)r : 8, h);
        lg("read(%d,%zu) %zd %s\n", fd, n, r, h);
    }
    return r;
}

int setsockopt(int fd, int lvl, int opt, const void *v, socklen_t l)
{
    static int (*real)(int, int, int, const void*, socklen_t);
    if (!real) real = dlsym(RTLD_NEXT, "setsockopt");
    int r = real(fd, lvl, opt, v, l);
    lg("setsockopt(%d, %d, %d) = %d\n", fd, lvl, opt, r);
    return r;
}

int ioctl(int fd, unsigned long req, ...)
{
    static int (*real)(int, unsigned long, ...);
    if (!real) real = dlsym(RTLD_NEXT, "ioctl");
    void *p = NULL;
    va_list ap; va_start(ap, req); p = va_arg(ap, void*); va_end(ap);
    int r = real(fd, req, p);
    lg("ioctl(%d, 0x%lx) = %d\n", fd, req, r);
    return r;
}

int open(const char *path, int flags, ...)
{
    static int (*real)(const char*, int, ...);
    if (!real) real = dlsym(RTLD_NEXT, "open");
    mode_t mode = 0;
    if (flags & 0x40) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    int r = real(path, flags, mode);
    if (r >= 0 && strcmp(path, "/tmp/shim.log")) lg("open(\"%s\") = %d\n", path, r);
    return r;
}
