#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdint.h>

#include "dtask.h"

#ifndef DTASK_GEN
#include "midi_tasks.h"
#endif

static midi_tasks_state_t state = DTASK_STATE(midi_tasks, 0, 0);
static dtask_set_t initial = 0;


int main(int argc, char *argv[]) {
  int status;
  snd_rawmidi_t *midiin = NULL;
  const char *portname = "hw:1,0,0";
  if ((argc > 1) && (strncmp("hw:", argv[1], 3) == 0)) {
    portname = argv[1];
  }
  if ((status = snd_rawmidi_open(&midiin, NULL, portname, SND_RAWMIDI_SYNC)) < 0) {
    printf("Problem opening MIDI input: %s\n", snd_strerror(status));
    exit(1);
  }

  char c;
  while(true) {
    if ((status = snd_rawmidi_read(midiin, &c, 1)) < 0) {
      printf("Problem reading MIDI input: %s\n", snd_strerror(status));
    }
    if ((unsigned char)c >= 0x80) { // command byte: print in hex
      printf("0x%x ", (unsigned char)buffer[0]);
    } else {
      printf("%d ", (unsigned char)buffer[0]);
    }
    fflush(stdout);
    if (count % 20 == 0) { // print a newline to avoid line-wrapping
      printf("\n");
    }
  }

  snd_rawmidi_close(midiin);
  midiin = NULL; // snd_rawmidi_close() does not clear invalid pointer,
  return 0;      // so might be a good idea to erase it after closing.
}
