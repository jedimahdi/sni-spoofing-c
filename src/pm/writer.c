#include "writer.h"
#include <string.h>
#include "ringbuf.h"

static inline size_t pm_min(size_t a, size_t b) {
  return a < b ? a : b;
}

void pm_writer_init(pm_writer *w, int fd, size_t capacity) {
  PM_ASSERT(w);
  pm_ringbuf_init(&w->rb, capacity);
  w->fd = fd;
  w->error = false;
  w->last_err = pm_ok();
}

void pm_writer_reset(pm_writer *w) {
  pm_ringbuf_reset(&w->rb);
  w->error = false;
  w->last_err = pm_ok();
}

void pm_writer_free(pm_writer *w) {
  pm_ringbuf_free(&w->rb);
  memset(w, 0, sizeof(*w));
}

pm_write_status pm_writer_flush(pm_writer *w) {
  if (w->error) return PM_WRITE_ERR;

  pm_span used_spans[2];
  pm_ringbuf_get_used_spans(&w->rb, used_spans);

  if (used_spans[0].len == 0) return PM_WRITE_OK;

  pm_write_result wr;
  if (used_spans[1].len == 0) {
    wr = pm_write(w->fd, used_spans[0].ptr, used_spans[0].len);
  } else {
    wr = pm_writev(w->fd, used_spans, 2);
  }

  switch (wr.status) {
  case PM_WRITE_OK:
  case PM_WRITE_WOULD_BLOCK:
    if (wr.nbytes > 0) pm_ringbuf_advance_head(&w->rb, wr.nbytes);
    return wr.status;
  case PM_WRITE_ERR:
  default:
    w->error = true;
    w->last_err = wr.err;
    return PM_WRITE_ERR;
  }
}

pm_write_result pm_writer_append_bytes(pm_writer *w, const void *data, size_t size) {
  PM_ASSERT(w && w->rb.cap > 0);
  pm_write_result res = {PM_WRITE_OK, 0, pm_ok()};

  if (w->error) {
    res.err = w->last_err;
    res.status = PM_WRITE_ERR;
    return res;
  }

  if (size == 0) return res;

  size_t used = pm_ringbuf_total_used(&w->rb);
  size_t needed_cap = used + size;

  if (w->rb.cap < needed_cap) {
    size_t min_new_cap = w->rb.cap;
    while (min_new_cap < needed_cap) {
      min_new_cap *= 2;
    }

    if (!pm_ringbuf_grow(&w->rb, min_new_cap)) {
      w->error = true;
      w->last_err = (pm_err){PM_ERR_NOMEM, 0};
      res.status = PM_WRITE_ERR;
      res.err = w->last_err;
      return res;
    }
  }

  pm_span free_spans[2];
  pm_ringbuf_get_free_spans(&w->rb, free_spans);

  size_t first_copy = pm_min(size, free_spans[0].len);
  memcpy(free_spans[0].ptr, data, first_copy);
  pm_ringbuf_advance_tail(&w->rb, first_copy);

  size_t remaining = size - first_copy;
  if (remaining > 0) {
    memcpy(free_spans[1].ptr, (const uint8_t *)data + first_copy, remaining);
    pm_ringbuf_advance_tail(&w->rb, remaining);
  }

  res.nbytes = size;
  return res;
}

pm_write_result pm_writer_write_bytes(pm_writer *w, const void *data, size_t size) {
  pm_write_result res = {PM_WRITE_OK, 0, pm_ok()};

  if (w->error) {
    res.status = PM_WRITE_ERR;
    res.err = w->last_err;
    return res;
  }

  if (size == 0) return res;

  pm_span used_spans[2];
  pm_ringbuf_get_used_spans(&w->rb, used_spans);
  size_t buffered = used_spans[0].len + used_spans[1].len;

  pm_write_result wr;
  if (buffered == 0) {
    wr = pm_write(w->fd, data, size);
  } else if (used_spans[1].len > 0) {
    pm_span spans[3] = {0};
    spans[0] = used_spans[0];
    spans[1] = used_spans[1];
    spans[2].ptr = (void *)data;
    spans[2].len = size;

    wr = pm_writev(w->fd, spans, 3);
  } else {
    pm_span spans[2] = {0};
    spans[0] = used_spans[0];
    spans[1].ptr = (void *)data;
    spans[1].len = size;

    wr = pm_writev(w->fd, spans, 2);
  }

  if (wr.status == PM_WRITE_ERR) {
    w->error = true;
    w->last_err = wr.err;
    return wr;
  }

  size_t consumed_from_buffer = pm_min(wr.nbytes, buffered);
  if (consumed_from_buffer > 0) pm_ringbuf_advance_head(&w->rb, consumed_from_buffer);

  size_t consumed_from_input = 0;
  if (wr.nbytes > buffered) {
    consumed_from_input = wr.nbytes - buffered;
    PM_ASSERT(consumed_from_input <= size);
  }

  size_t remaining = size - consumed_from_input;
  if (remaining == 0) {
    res.status = wr.status;
    res.nbytes = size;
    return res;
  }

  pm_write_result ar = pm_writer_append_bytes(w, (const uint8_t *)data + consumed_from_input, remaining);

  if (ar.status == PM_WRITE_ERR) {
    res.status = PM_WRITE_ERR;
    res.err = ar.err;
    res.nbytes = consumed_from_input + ar.nbytes;
    return res;
  }

  if (ar.nbytes == remaining) {
    res.status = PM_WRITE_OK;
    res.nbytes = size;
    return res;
  }

  res.status = PM_WRITE_WOULD_BLOCK;
  res.nbytes = consumed_from_input + ar.nbytes;
  return res;
}

pm_write_result pm_writer_write_cstr(pm_writer *w, const char *s) {
  return pm_writer_write_bytes(w, s, strlen(s));
}

pm_write_result pm_writer_append_cstr(pm_writer *w, const char *s) {
  return pm_writer_append_bytes(w, s, strlen(s));
}

pm_write_result pm_writer_write_strn(pm_writer *w, const char *s, size_t len) {
  return pm_writer_write_bytes(w, s, len);
}

bool pm_writer_is_empty(const pm_writer *w) {
  return pm_ringbuf_is_empty(&w->rb);
}

bool pm_writer_is_full(const pm_writer *w) {
  return pm_ringbuf_is_full(&w->rb);
}

size_t pm_writer_avail(const pm_writer *w) {
  return pm_ringbuf_total_free(&w->rb);
}

size_t pm_writer_size(const pm_writer *w) {
  return pm_ringbuf_total_used(&w->rb);
}
