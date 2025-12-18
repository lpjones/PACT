#ifndef _FIFO_H
#define _FIFO_H

#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>

#include "pact.h"

struct fifo_list {
  struct pact_page *first, *last;
  pthread_mutex_t list_lock;
  size_t numentries;
};


void enqueue_fifo(struct fifo_list *list, struct pact_page *page);
struct pact_page* dequeue_fifo(struct fifo_list *list);
void page_list_remove_page(struct fifo_list *list, struct pact_page *page);
void next_page(struct fifo_list *list, struct pact_page *page, struct pact_page **res);

#endif

