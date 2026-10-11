#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
  uint64_t id;
  uint64_t expire_at_ms;
  void *user_data;
} timer_node;

typedef struct {
  timer_node *nodes;
  int capacity;
  int count;
  uint64_t next_id;
} timer_heap;

uint64_t get_time_ms(void);
void timer_heap_init(timer_heap *heap, int initial_capacity);
void timer_heap_free(timer_heap *heap);
uint64_t timer_heap_push(timer_heap *heap, uint64_t expire_at_ms, void *user_data);
timer_node *timer_heap_peak(timer_heap *heap);
timer_node timer_heap_pop(timer_heap *heap);
bool timer_heap_remove(timer_heap *heap, uint64_t id);
