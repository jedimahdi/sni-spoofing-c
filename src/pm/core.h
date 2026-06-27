#ifndef PM_CORE_H
#define PM_CORE_H

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#ifndef PM_ASSERT
#include <assert.h>
#define PM_ASSERT assert
#endif

#ifndef PM_LOG_ERROR
#include <stdio.h>
#define PM_LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] [PM]: " fmt "\n", ##__VA_ARGS__)
#endif

#define PM_MAX(a, b) ((a) > (b) ? (a) : (b))
#define PM_MIN(a, b) ((a) < (b) ? (a) : (b))

typedef enum {
  PM_OK = 0,
  PM_ERR_INVAL,
  PM_ERR_IO,
  PM_ERR_PLATFORM,
  PM_ERR_AGAIN,
  PM_ERR_NOMEM,
  PM_ERR_PIPE,
  PM_ERR_NOENT,
  PM_ERR_RANGE,
  PM_ERR_PERM,
} pm_error;

typedef struct {
  pm_error code;
  int sys_errno;
} pm_err;

pm_err pm_err_from_errno(int e);
static inline pm_err pm_ok(void) {
  return (pm_err){PM_OK, 0};
}
static inline bool pm_err_is_ok(pm_err e) {
  return e.code == PM_OK;
}

pm_err pm_set_nonblocking(int fd);

typedef enum {
  PM_READ_OK,
  PM_READ_ERR,
  PM_READ_WOULD_BLOCK,
  PM_READ_EOF,
} pm_read_status;

typedef enum {
  PM_WRITE_OK,
  PM_WRITE_ERR,
  PM_WRITE_WOULD_BLOCK,
} pm_write_status;

typedef struct {
  pm_read_status status;
  size_t nbytes;
  pm_err err;
} pm_read_result;

typedef struct {
  pm_write_status status;
  size_t nbytes;
  pm_err err;
} pm_write_result;

pm_read_result pm_read(int fd, void *buf, size_t nbyte);
pm_write_result pm_write(int fd, const void *buf, size_t nbyte);

#define PM_IOV_MAX 8

typedef struct {
  void *ptr;
  size_t len;
} pm_span;

pm_read_result pm_readv(int fd, const pm_span *spans, int span_count);
pm_write_result pm_writev(int fd, const pm_span *spans, int span_count);

#endif
