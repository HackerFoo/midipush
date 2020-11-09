/* Copyright 2020-2020 Dustin DeWeese
   This file is part of MidiPush.

    MidiPush is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    MidiPush is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with MidiPush.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/time.h>
#include <poll.h>
#include <stdarg.h>

#include "dtask.h"
#include "types.h"

#include "startle/types.h"
#include "startle/macros.h"
#include "startle/support.h"
#include "startle/log.h"
#include "startle/error.h"
#include "startle/test.h"
#include "startle/map.h"
#include "startle/stats_types.h"
#include "startle/stats.h"
#include "startle/static_alloc.h"

#include "midi_tasks.h"
#include "midipush.h"

const int initial = PRINT_MIDI_MSG | LIGHT_BAR | PLAYBACK | SHOW_PROGRAM | PASSTHROUGH | SHOW_DISABLE_CHANNEL;

static snd_rawmidi_t *rawmidi_in = NULL, *rawmidi_out = NULL, *synth_out = NULL;

static char push_init[] = {
  0xF0, 0x47, 0x7F, 0x15, 0x63, 0x00, 0x01, 0x05, 0xF7,
  0xF0, 0x47, 0x7F, 0x15, 0x5C, 0x00, 0x01, 0x01, 0xF7,
  0xF0, 0x47, 0x7F, 0x15, 0x01, 0x01, 0xF7, 0xF0, 0x47, 0x7F, 0x15, 0x5D, 0x00, 0x20, 0x00, 0x00, 0x0B, 0x0E, 0x00, 0x00, 0x0D, 0x02, 0x00, 0x00, 0x00, 0x01, 0x04, 0x0C, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x0D, 0x04, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0E, 0x0A, 0x06, 0x00, 0xF7,
  0xF0, 0x47, 0x7F, 0x15, 0x62, 0x00, 0x01, 0x00, 0xF7, // live mode
  0xF0, 0x7E, 0x00, 0x06, 0x01, 0xF7,
  0xF0, 0x47, 0x7F, 0x15, 0x57, 0x00, 0x14, 0x00, 0x00, 0x0D, 0x07, 0x00, 0x03, 0x0E, 0x08, 0x00, 0x00, 0x0C, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x08, 0xF7,
  0xF0, 0x47, 0x7F, 0x15, 0x5D, 0x00, 0x20, 0x00, 0x00, 0x0B, 0x0E, 0x00, 0x00, 0x0D, 0x02, 0x00, 0x00, 0x00, 0x01, 0x04, 0x0C, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x0D, 0x04, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0E, 0x0A, 0x06, 0x00, 0xF7,
  0xF0, 0x47, 0x7F, 0x15, 0x62, 0x00, 0x01, 0x00, 0xF7,
  0xF0, 0x47, 0x7F, 0x15, 0x5C, 0x00, 0x01, 0x01, 0xF7,
  0xF0, 0x47, 0x7F, 0x15, 0x60, 0x00, 0x04, 0x41, 0x09, 0x02, 0x03, 0xF7,
  0xF0, 0x47, 0x7F, 0x15, 0x50, 0x00, 0x10, 0x00, 0x00, 0x00, 0x0A, 0x0D, 0x05, 0x06, 0x07, 0x00, 0x00, 0x02, 0x01, 0x03, 0x08, 0x03, 0x05, 0xF7,
  0xF0, 0x47, 0x7F, 0x15, 0x47, 0x00, 0x09, 0x00, 0x00, 0x03, 0x02, 0x00, 0x01, 0x0F, 0x04, 0x50, 0xF7,
};

static midi_tasks_state_t midi_state = DTASK_STATE(midi_tasks, 0, 0);

STATIC_ALLOC(midi_input_buffer, char, 1024);
STATIC_ALLOC_DEPENDENT(midi_input_rb_buffer, char, sizeof(ring_buffer_t) + static_sizeof(midi_input_buffer));
static ring_buffer_t *midi_input_rb;

int fixed_length(unsigned char c) {
  int
    l = (c >> 4) & 7, // left 3 bits
    r = (c & 7); // right 4 bits

  if(l != 7) {
    return (l & 6) == 4 ? 2 : 3; // channel voice messages
  } else if(r & 12) {
    return 1; // real time messages, undefined, tune request, end of exclusive
  } else if(r) {
    return r & 1 ? 2 : 3; // time code quarter frame, song position pointer, song select
  } else { // sysex
    return -1;
  }
}

seg_t find_midi_msg(const char **s, const char *e) {

  // skip to first control byte
  const char *p = *s;
  while(p < e &&
        !((unsigned char)*p & 0x80)) {
    printf("* 0x%x\n", *p);
    p++;
  }
  if(p == e) {
    *s = e;
    return (seg_t) {0};
  }
  seg_t msg = { .s = p, .n = 0 };

  int len = fixed_length(*p);
  if(len < 0) { // sysex
    p++;
    msg.n++;
    while(p < e) {
      msg.n++;
      if((unsigned char)*p == 0xf7) {
        *s = p + 1;
        return msg;
      }
    }
    return (seg_t) {0};
  } else {
    msg.n = len;
  }

  if(e - p < msg.n) {
    return (seg_t) {0};
  }

  *s = p + msg.n;
  return msg;
}

static
bool read_midi_msgs(snd_rawmidi_t *m, ring_buffer_t *rb, midi_tasks_state_t *state) {
  bool success = true;
  while(success) {

    // read from ring buffer first
    const char *buffer = midi_input_buffer;
    size_t remaining = static_sizeof(midi_input_buffer);
    ssize_t n = rb_read(rb, buffer, remaining);

    // fill remainder from input
    ssize_t r = snd_rawmidi_read(m, buffer + n, remaining - n);
    if(r < 0) {
      success = r == -EAGAIN;
      break;
    } else {
      n += r;
    }

    const char *buffer_end = buffer + n;
    seg_t msg;
    while(msg = find_midi_msg(&buffer, buffer_end), msg.n) {
      midi_state.push_midi_msg_in = msg;
      dtask_run((dtask_state_t *)state, PUSH_MIDI_MSG_IN);
      midi_state.msg_count++;
    }

    // push remaining bytes into the ring buffer
    n = rb_write(rb, buffer, buffer_end - buffer);
    success &= n == buffer_end - buffer;
  }
  return success;
}

void write_midi(seg_t s) {
  snd_rawmidi_write(rawmidi_out, s.s, s.n);
}

void write_synth(seg_t s) {
  snd_rawmidi_write(synth_out, s.s, s.n);
}

void synth_note(uint8_t channel, uint8_t note, bool on, uint8_t pressure) {
  channel &= 0x0f;
  write_synth((seg_t) {
    .n = 3,
    .s = (char [3]) {
      (on ? 0x90 : 0x80) | channel,
      note,
      pressure
    }
  });
  printf("synth %d %s %d\n", (int)note, on ? "on" : "off", (int)pressure);
}

void set_pad_rgb_color(unsigned int pad, unsigned int rgb) {
  int
    r = (rgb >> 16) & 0xff,
    g = (rgb >>  8) & 0xff,
    b = rgb & 0xff;
  write_midi((seg_t) {
    .n = 16,
    .s = (char [16]) {
      0xf0,    0x47,   0x7f,    0x15,
      0x04,    0x00,   0x08,    pad,
      0,       r >> 4, r & 0xf, g >> 4,
      g & 0xf, b >> 4, b & 0xf, 0xf7
    }
  });
}

void send_msg(int c, int x, int y) {
  int len = fixed_length(c);
  if(len >= 0) {
    write_midi((seg_t) {
      .n = len,
      .s = (char [3]) {c, x, y}
    });
  }
}

void set_pad_color(unsigned int pad, unsigned int color) {
  send_msg(0x90, pad + 36, color);
}

void all_notes_off(int channel) {
  write_synth((seg_t) {
    .n = 3,
    .s = (char [3]) { 0xb0 | (channel & 0x0f), 0x7b, 0 }
  });
}

//                        A  #  B   C  #  D  #  E  F  #  G  #
uint8_t background[12] = {2, 0, 2, 45, 0, 2, 0, 2, 2, 0, 2, 0};
uint8_t background_offset = 0;
const char note_name[24] = "A A#B C C#D D#E F F#G G#";

char *get_note_name(unsigned int note) {
  return &note_name[2 * ((note + 10) % 12)];
}

int get_note_octave(unsigned int note) {
  return (note + 10) / 12;
}

uint8_t background_color(unsigned int note) {
  return background[(note + 10 + background_offset) % LENGTH(background)];
}

unsigned int pad_to_note(unsigned int pad) {
  int
    x = pad & 7,
    y = pad >> 3,
    block_row = y >> 2,
    block_column = (x + 2) / 3,
    block_x = (x + 2) % 3,
    block_y = y & 3;
  return block_row * 24 + block_column * 12 + block_x * 4 + block_y + 2 - 10;
}

void write_text(int x, int y, seg_t s) {
  write_midi((seg_t) {
    .n = 8,
    .s = (char[8]) {0xf0, 0x47, 0x7f, 0x15, 0x18 + y, 0, s.n + 1, x}
  });
  write_midi(s);
  write_midi((seg_t) { .n = 1, .s = (char[1]) {0xf7} });
}

int printf_text(int x, int y, const char *fmt, ...) {
  va_list args;
  static char text[80];
  va_start(args, fmt);
  int n = vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  write_text(x, y, (seg_t) { .n = n, .s = text });
}

int find_card(char *query) {
  int card = -1;
  char *name;
  while(!snd_card_next(&card) && card >= 0) {
    if(!snd_card_get_name(card, &name)) {
      //printf("found: %s\n", name);
      if(!strcmp(name, query)) return card;
    }
  }
  return -1;
}

static struct pollfd pfds_in[16];
static int pfds_in_n = 0;
static
int get_pfds(snd_rawmidi_t *m, struct pollfd *pfds, int pfds_n) {
  int count = snd_rawmidi_poll_descriptors_count(rawmidi_in);
  assert_throw(count <= pfds_n, "pfds_n not large enough\n");
  count = snd_rawmidi_poll_descriptors(m, pfds, pfds_n);

  // keep only input pfds
  int n = 0;
  COUNTUP(i, count) {
    struct pollfd pfd = pfds[i];
    if(pfd.events & POLLIN) {
      pfds[n++] = pfd;
    }
  }
  return n;
}

STATIC_ALLOC(record, pair_t, 1 << 15);
int main(int argc, char *argv[]) {
  static_alloc_init();
  log_init();
  midi_input_rb = rb_init(midi_input_rb_buffer, static_sizeof(midi_input_rb_buffer));

  error_t test_error;
  CATCH(&test_error) {
    printf(NOTE("ERROR") " ");
    print_last_log_msg();
    return -1;
  } else {
    int card = find_card("Ableton Push");
    assert_throw(card >= 0, "Ableton Push not found.");
    int synth = find_card("VirMIDI");
    assert_throw(synth >= 0, "Synth not found.");

    ssize_t n;
    rawmidi_in = NULL;
    rawmidi_out = NULL;
    char portname[16];

    // open push
    snprintf(portname, sizeof(portname), "hw:%d,0,0", card);
    n = snd_rawmidi_open(&rawmidi_in, &rawmidi_out, portname, SND_RAWMIDI_SYNC | SND_RAWMIDI_NONBLOCK);
    assert_throw(n >= 0, "Problem opening MIDI port: %s", snd_strerror(n));
    COUNTUP(i, sizeof(push_init)) {
      char c = push_init[i];
      n = snd_rawmidi_write(rawmidi_out, &c, 1);
      assert_throw(n >= 0, "Problem sending initialization: %s", snd_strerror(n));
      if((unsigned char)c & 0x80) usleep(1000);
    }

    // open synth
    snprintf(portname, sizeof(portname), "hw:%d,0,0", synth);
    n = snd_rawmidi_open(NULL, &synth_out, portname, SND_RAWMIDI_SYNC | SND_RAWMIDI_NONBLOCK);
    assert_throw(n >= 0, "Problem opening MIDI port: %s", snd_strerror(n));

    pfds_in_n = get_pfds(rawmidi_in, pfds_in, LENGTH(pfds_in));
    dtask_enable((dtask_state_t *)&midi_state, initial);
    dtask_select((dtask_state_t *)&midi_state);
    midi_state.record = init_map(record, record_size);
    while(read_midi_msgs(rawmidi_in, midi_input_rb, &midi_state)) {
      poll(pfds_in, pfds_in_n, 10);
      gettimeofday(&midi_state.time_of_day, NULL);
      dtask_run((dtask_state_t *)&midi_state, TIME_OF_DAY);
    }

    snd_rawmidi_close(rawmidi_in);
    snd_rawmidi_close(rawmidi_out);
    return 0;
  }
}
