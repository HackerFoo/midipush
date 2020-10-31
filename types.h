#ifndef __TYPES__
#define __TYPES__
#include <stdint.h>

#define MAX_MIDI_DATA_LENGTH 7

struct midi_msg {
  int data_length;
  uint8_t control;
  uint8_t data[MAX_MIDI_DATA_LENGTH];
};

#endif
