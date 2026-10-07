#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "event.h"

#define EVENT_QUEUE_DEPTH 100

K_MSGQ_DEFINE(eventQueue, sizeof(event_t), EVENT_QUEUE_DEPTH, 4);

void eventQueuePush(event_t event)
{
  if (k_msgq_put(&eventQueue, &event, K_NO_WAIT) != 0) {
    printk("[event] queue full, dropped %d\n", event);
  }
}

event_t eventQueueWait(void)
{
  event_t event = EVENT_NONE;
  k_msgq_get(&eventQueue, &event, K_FOREVER);
  return event;
}
