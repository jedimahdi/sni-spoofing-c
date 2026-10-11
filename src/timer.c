#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>

uint64_t get_time_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

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

void timer_heap_init(timer_heap *heap, int initial_capacity) {
  heap->nodes = malloc(initial_capacity * sizeof(*heap->nodes));
  heap->count = 0;
  heap->capacity = initial_capacity;
  heap->next_id = 1;
}

void timer_heap_free(timer_heap *heap) {
  free(heap->nodes);
  heap->nodes = NULL;
  heap->count = 0;
  heap->capacity = 0;
}

static void swap_nodes(timer_heap *heap, int i, int j) {
  timer_node temp = heap->nodes[i];
  heap->nodes[i] = heap->nodes[j];
  heap->nodes[j] = temp;
}

static void sift_up(timer_heap *heap, int index) {
  while (index > 0) {
    int parent = (index - 1) / 2;
    if (heap->nodes[index].expire_at_ms < heap->nodes[parent].expire_at_ms) {
      swap_nodes(heap, index, parent);
      index = parent;
    } else {
      break;
    }
  }
}

static void sift_down(timer_heap *heap, int index) {
  int last = heap->count - 1;

  while (true) {
    int left = 2 * index + 1;
    int right = 2 * index + 2;
    int smallest = index;

    if (left <= last && heap->nodes[left].expire_at_ms < heap->nodes[smallest].expire_at_ms) {
      smallest = left;
    }
    if (right <= last && heap->nodes[right].expire_at_ms < heap->nodes[smallest].expire_at_ms) {
      smallest = right;
    }
    if (smallest != index) {
      swap_nodes(heap, index, smallest);
      index = smallest;
    } else {
      break;
    }
  }
}

uint64_t timer_heap_push(timer_heap *heap, uint64_t expire_at_ms, void *user_data) {
  if (heap->count >= heap->capacity) {
    heap->capacity *= 2;
    heap->nodes = realloc(heap->nodes, heap->capacity * sizeof(*heap->nodes));
  }

  uint64_t id = heap->next_id++;
  heap->nodes[heap->count].id = id;
  heap->nodes[heap->count].expire_at_ms = expire_at_ms;
  heap->nodes[heap->count].user_data = user_data;

  sift_up(heap, heap->count);
  heap->count++;
  return id;
}

timer_node *timer_heap_peak(timer_heap *heap) {
  if (heap->count == 0) return NULL;
  return &heap->nodes[0];
}

timer_node timer_heap_pop(timer_heap *heap) {
  timer_node result = heap->nodes[0];
  heap->count--;

  if (heap->count > 0) {
    heap->nodes[0] = heap->nodes[heap->count];
    sift_down(heap, 0);
  }

  return result;
}

bool timer_heap_remove(timer_heap *heap, uint64_t id) {
  for (int i = 0; i < heap->count; i++) {
    if (heap->nodes[i].id == id) {
      heap->count--;

      if (i < heap->count) {
        heap->nodes[i] = heap->nodes[heap->count];
        sift_up(heap, i);
        sift_down(heap, i);
      }

      return true;
    }
  }
  return false;
}

