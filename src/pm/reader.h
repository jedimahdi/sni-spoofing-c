#ifndef PM_READER_H
#define PM_READER_H

#include "core.h"
#include "ringbuf.h"

typedef struct {
  int fd;
  pm_ringbuf rb;
  pm_err last_err;
  bool eof;
  bool error;
} pm_reader;

void pm_reader_init(pm_reader *r, int fd, size_t capacity);
void pm_reader_free(pm_reader *r);
void pm_reader_reset(pm_reader *r);
void pm_reader_dump(const pm_reader *r);

size_t pm_reader_total_avail_data(const pm_reader *r);
size_t pm_reader_total_free_space(const pm_reader *r);
pm_err pm_reader_error(const pm_reader *r);
bool pm_reader_is_eof(const pm_reader *r);
bool pm_reader_is_empty(const pm_reader *r);

pm_read_status pm_reader_fill(pm_reader *r);

pm_read_result pm_reader_skip(pm_reader *r, size_t n);
pm_read_status pm_reader_need_more_contiguous(pm_reader *r);
pm_read_status pm_reader_ensure_contiguous(pm_reader *r, size_t needed);
void pm_reader_compact(pm_reader *r);

pm_read_result pm_reader_read(pm_reader *r, void *buf, size_t nbyte);
const uint8_t *pm_reader_peek(const pm_reader *r, size_t *avail_out);

#endif
