#include "core.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/uio.h>

pm_err pm_err_from_errno(int e) {
  switch (e) {
  case 0:
    return (pm_err){PM_OK, 0};
  case EAGAIN:
    // #ifdef EWOULDBLOCK
    //   case EWOULDBLOCK:
    // #endif
    return (pm_err){PM_ERR_AGAIN, e};
  case EINVAL:
  case EBADF:
    return (pm_err){PM_ERR_INVAL, e};
  case ENOMEM:
    return (pm_err){PM_ERR_NOMEM, e};
  case EIO:
    return (pm_err){PM_ERR_IO, e};
  case EPIPE:
    return (pm_err){PM_ERR_PIPE, e};
  case EACCES:
  case EPERM:
    return (pm_err){PM_ERR_PERM, e};
  case ENOENT:
  case ENOTDIR:
    return (pm_err){PM_ERR_NOENT, e};
  case ERANGE:
    return (pm_err){PM_ERR_RANGE, e};
  default:
    return (pm_err){PM_ERR_PLATFORM, e};
  }
}

pm_err pm_set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL);
  if (flags == -1) return pm_err_from_errno(errno);
  if (flags & O_NONBLOCK) return pm_ok();
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
    return pm_err_from_errno(errno);
  return pm_ok();
}

pm_read_result pm_read(int fd, void *buf, size_t nbyte) {
  PM_ASSERT(buf);

  if (nbyte == 0) {
    return (pm_read_result){PM_READ_OK, 0, pm_ok()};
  }

  ssize_t n;

  do {
    n = read(fd, buf, nbyte);
  } while (n < 0 && errno == EINTR);

  if (n == 0)
    return (pm_read_result){PM_READ_EOF, 0, pm_ok()};

  if (n < 0) {
    if (errno == EWOULDBLOCK || errno == EAGAIN)
      return (pm_read_result){PM_READ_WOULD_BLOCK, 0, pm_err_from_errno(errno)};

    PM_LOG_ERROR("read failed: %s", strerror(errno));
    return (pm_read_result){PM_READ_ERR, 0, pm_err_from_errno(errno)};
  }

  return (pm_read_result){PM_READ_OK, (size_t)n, pm_ok()};
}

pm_write_result pm_write(int fd, const void *buf, size_t nbyte) {
  PM_ASSERT(buf);

  if (nbyte == 0) {
    return (pm_write_result){PM_WRITE_OK, 0, pm_ok()};
  }

  ssize_t n;

  do {
    n = write(fd, buf, nbyte);
  } while (n < 0 && errno == EINTR);

  if (n < 0) {
    if (errno == EWOULDBLOCK || errno == EAGAIN)
      return (pm_write_result){PM_WRITE_WOULD_BLOCK, 0, pm_err_from_errno(errno)};

    PM_LOG_ERROR("write failed: %s", strerror(errno));
    return (pm_write_result){PM_WRITE_ERR, 0, pm_err_from_errno(errno)};
  }

  return (pm_write_result){PM_WRITE_OK, (size_t)n, pm_ok()};
}

pm_read_result pm_readv(int fd, const pm_span *iov, int iovcnt) {
  PM_ASSERT(iov && iovcnt > 0 && iovcnt <= PM_IOV_MAX);

  struct iovec sysiov[PM_IOV_MAX];
  for (int i = 0; i < iovcnt; ++i) {
    sysiov[i].iov_base = iov[i].ptr;
    sysiov[i].iov_len = iov[i].len;
  }

  ssize_t n;
  do {
    n = readv(fd, sysiov, iovcnt);
  } while (n < 0 && errno == EINTR);

  if (n == 0) return (pm_read_result){PM_READ_EOF, 0, pm_ok()};

  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return (pm_read_result){PM_READ_WOULD_BLOCK, 0, pm_err_from_errno(errno)};

    PM_LOG_ERROR("readv failed: %s", strerror(errno));
    return (pm_read_result){PM_READ_ERR, 0, pm_err_from_errno(errno)};
  }
  return (pm_read_result){PM_READ_OK, (size_t)n, pm_ok()};
}

pm_write_result pm_writev(int fd, const pm_span *iov, int iovcnt) {
  PM_ASSERT(iov && iovcnt > 0 && iovcnt <= PM_IOV_MAX);

  struct iovec sysiov[PM_IOV_MAX];
  for (int i = 0; i < iovcnt; i++) {
    sysiov[i].iov_base = (void *)iov[i].ptr;
    sysiov[i].iov_len = iov[i].len;
  }

  ssize_t n;
  do {
    n = writev(fd, sysiov, iovcnt);
  } while (n < 0 && errno == EINTR);

  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return (pm_write_result){PM_WRITE_WOULD_BLOCK, 0, pm_err_from_errno(errno)};

    PM_LOG_ERROR("writev failed: %s", strerror(errno));
    return (pm_write_result){PM_WRITE_ERR, 0, pm_err_from_errno(errno)};
  }
  return (pm_write_result){PM_WRITE_OK, (size_t)n, pm_ok()};
}
