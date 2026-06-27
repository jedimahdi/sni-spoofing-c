#include "reader.h"
#include <stdlib.h>
#include <string.h>
#include "ringbuf.h"

static inline size_t pm_min(size_t a, size_t b) {
  return a < b ? a : b;
}

void pm_reader_init(pm_reader *r, int fd, size_t capacity) {
  PM_ASSERT(r);
  pm_ringbuf_init(&r->rb, capacity);
  r->fd = fd;
  r->last_err = pm_ok();
  r->eof = false;
  r->error = false;
}

void pm_reader_free(pm_reader *r) {
  PM_ASSERT(r);
  pm_ringbuf_free(&r->rb);
  memset(r, 0, sizeof(*r));
}

void pm_reader_reset(pm_reader *r) {
  PM_ASSERT(r);
  pm_ringbuf_reset(&r->rb);
  r->eof = false;
  r->error = false;
  r->last_err = pm_ok();
}

bool pm_reader_is_empty(const pm_reader *r) {
  return pm_ringbuf_is_empty(&r->rb);
}

bool pm_reader_is_eof(const pm_reader *r) {
  return pm_reader_is_empty(r) && r->eof;
}

size_t pm_reader_total_avail_data(const pm_reader *r) {
  return pm_ringbuf_total_used(&r->rb);
}

size_t pm_reader_total_free_space(const pm_reader *r) {
  return pm_ringbuf_total_free(&r->rb);
}

pm_err pm_reader_error(const pm_reader *r) {
  return r->last_err;
}

pm_read_status pm_reader_fill(pm_reader *r) {
  if (r->error) return PM_READ_ERR;
  if (r->eof) return PM_READ_EOF;

  pm_span free_spans[2];
  pm_ringbuf_get_free_spans(&r->rb, free_spans);

  if (free_spans[0].len == 0) return PM_READ_OK;

  pm_read_result rr;
  if (free_spans[1].len == 0) {
    rr = pm_read(r->fd, free_spans[0].ptr, free_spans[0].len);
  } else {
    rr = pm_readv(r->fd, free_spans, 2);
  }

  switch (rr.status) {
  case PM_READ_OK:
    pm_ringbuf_advance_tail(&r->rb, rr.nbytes);
    return PM_READ_OK;
  case PM_READ_WOULD_BLOCK:
    pm_ringbuf_advance_tail(&r->rb, rr.nbytes);
    return PM_READ_WOULD_BLOCK;
  case PM_READ_EOF:
    r->eof = true;
    return PM_READ_EOF;
  case PM_READ_ERR:
  default:
    r->error = true;
    r->last_err = rr.err;
    return PM_READ_ERR;
  }
}

const uint8_t *pm_reader_peek(const pm_reader *r, size_t *avail_out) {
  PM_ASSERT(r);
  pm_span used_spans[2];
  pm_ringbuf_get_used_spans(&r->rb, used_spans);
  if (avail_out) *avail_out = used_spans[0].len;
  return used_spans[0].len == 0 ? NULL : used_spans[0].ptr;
}

static pm_read_result pm_reader_consume(pm_reader *r, void *dst, size_t n) {
  pm_read_result res = {PM_READ_OK, 0, pm_ok()};
  pm_span used_spans[2];

  while (res.nbytes < n) {
    pm_ringbuf_get_used_spans(&r->rb, used_spans);
    if (used_spans[0].len == 0) {
      pm_read_status st = pm_reader_fill(r);
      if (st != PM_READ_OK) {
        res.status = st;
        res.err = r->last_err;
        return res;
      }
      continue;
    }

    size_t to_take = pm_min(used_spans[0].len, n - res.nbytes);
    if (dst) {
      memcpy((uint8_t *)dst + res.nbytes, used_spans[0].ptr, to_take);
    }
    pm_ringbuf_advance_head(&r->rb, to_take);
    res.nbytes += to_take;
  }

  return res;
}

pm_read_result pm_reader_read(pm_reader *r, void *buf, size_t nbyte) {
  PM_ASSERT(r && buf);
  return pm_reader_consume(r, buf, nbyte);
}

pm_read_result pm_reader_skip(pm_reader *r, size_t n) {
  return pm_reader_consume(r, NULL, n);
}

void pm_reader_compact(pm_reader *r) {
  pm_ringbuf_compact(&r->rb);
}

pm_read_status pm_reader_need_more_contiguous(pm_reader *r) {
  pm_span used_spans[2];
  pm_ringbuf_get_used_spans(&r->rb, used_spans);

  if (used_spans[1].len > 0) {
    pm_ringbuf_compact(&r->rb);
    return PM_READ_OK;
  }

  if (pm_ringbuf_is_full(&r->rb)) {
    if (!pm_ringbuf_grow(&r->rb, r->rb.cap * 2)) {
      r->error = true;
      r->last_err = (pm_err){PM_ERR_NOMEM, 0};
      return PM_READ_ERR;
    }
  }

  return pm_reader_fill(r);
}

pm_read_status pm_reader_ensure_contiguous(pm_reader *r, size_t needed) {
  PM_ASSERT(r);

  while (pm_ringbuf_contig_used(&r->rb) < needed) {
    pm_read_status st = pm_reader_need_more_contiguous(r);
    if (st != PM_READ_OK) return st;
  }

  return PM_READ_OK;
}

void pm_reader_dump(const pm_reader *r) {
  printf("\n");
  printf("pm_reader:\n");
  printf("  cap: %zu\n", r->rb.cap);
  printf("  head: %zu\n", r->rb.head);
  printf("  tail: %zu\n", r->rb.tail);
  printf("  eof: %s\n", r->eof ? "true" : "false");
  size_t avail;
  const uint8_t *s = pm_reader_peek(r, &avail);
  printf("  avail: %zu\n", avail);
  if (s) {
    printf("  peek: `%.*s`\n", (int)avail, (const char *)s);
    printf("  peek hex:");
    for (size_t i = 0; i < avail; i++) {
      printf(" %02X", s[i]);
    }
    printf("\n");

  } else {
    printf("  peek: <empty>\n");
  }
  printf("\n");
}
