#include <stdio.h>
#include "types.h"

#ifndef DTASK_GEN
#include "midi_tasks.h"
#endif

DTASK_GROUP(midi_tasks)

// passthrough
DTASK(push_midi_msg_in, struct midi_msg) {
  return true;
}

DTASK(print_midi_msg, bool) {
  const push_midi_msg_in_t *msg = DREF(push_midi_msg_in);
  printf("0x%x:", msg->control);
  for(int i = 0; i < msg->data_length; i++) {
    printf(" %d", msg->data[i]);
  }
  printf("\n");
  return true;
}
