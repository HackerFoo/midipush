/* Copyright 2020-2021 Dustin DeWeese
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
#include <endian.h>
#include <zlib.h> // crc32

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

#include "vec128b.h"
#include "midi_tasks.h"
#include "midipush.h"

#define DEBUG 0

#define STATE_FILE "midipush.state"
#define MIDI_FILE  "midipush.mid"

const unsigned long long initial =
  PRINT_MIDI_MSG |
  LIGHT_BAR |
  PLAYBACK |
  SHOW_PROGRAM |
  PASSTHROUGH |
  SHOW_DISABLE_CHANNEL |
  TRANSPOSE |
  SHOW_VOLUME |
  SHOW_PLAYBACK |
  POWEROFF |
  SAVE |
  METRONOME;

struct midi {
  snd_rawmidi_t *in, *out;
  ring_buffer_t *rb;
  int id;
  char last_status; // to support MIDI running status, where repeated status bytes are omitted.
};

struct midi push, synth, ext;

struct write_midi_file {
  size_t track_size;
  int fd;
  unsigned int last_beat, current_beat;
  bool active;
};

static struct write_midi_file _write_midi_file;

static
void midi_open(struct midi *p, int id, int card, int device, char *buf, size_t buf_n) {
  char portname[16];
  snprintf(portname, sizeof(portname), "hw:%d,%d,0", card, device);
  int n = snd_rawmidi_open(&p->in, &p->out, portname, SND_RAWMIDI_SYNC | SND_RAWMIDI_NONBLOCK);
  assert_throw(n >= 0, "Problem opening MIDI port %s: %s", portname, snd_strerror(n));
  p->rb = rb_init(buf, buf_n);
  p->id = id;
}

static
void midi_close(struct midi *p) {
  snd_rawmidi_close(p->in);
  snd_rawmidi_close(p->out);
}

static char push_init[] = {
  0xF0, 0x47, 0x7F, 0x15, 0x63, 0x00, 0x01, 0x05, 0xF7, // touch strip mode
  0xF0, 0x47, 0x7F, 0x15, 0x5C, 0x00, 0x01, 0x01, 0xF7, // channel aftertouch
  0xF0, 0x47, 0x7F, 0x15, 0x62, 0x00, 0x01, 0x00, 0xF7, // live mode
  0xF0, 0x7E, 0x00, 0x06, 0x01, 0xF7,
  0xF0, 0x47, 0x7F, 0x15, 0x57, 0x00, 0x14, 0x00, 0x00, // calibration
        0x0D, 0x07, 0x00, 0x03, 0x0E, 0x08, 0x00, 0x00,
        0x0C, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x0C, 0x08, 0xF7,
  0xF0, 0x47, 0x7F, 0x15, 0x47, 0x00, 0x09, 0x00, 0x00, // pad parameter
        0x03, 0x02, 0x00, 0x01, 0x0F, 0x04, 0x50, 0xF7,
};

void set_pad_threshold(int x) {
  x = clamp(0, 31, x);
  const static uint8_t pad_thresh[32][7] = {
    { 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x0A },
    { 0x00, 0x01, 0x0C, 0x00, 0x00, 0x01, 0x0E },
    { 0x00, 0x02, 0x0E, 0x00, 0x00, 0x03, 0x02 },
    { 0x00, 0x03, 0x07, 0x00, 0x00, 0x03, 0x0C },
    { 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x06 },
    { 0x00, 0x04, 0x09, 0x00, 0x00, 0x05, 0x00 },
    { 0x00, 0x05, 0x02, 0x00, 0x00, 0x05, 0x0A },
    { 0x00, 0x05, 0x0B, 0x00, 0x00, 0x06, 0x04 },
    { 0x00, 0x06, 0x04, 0x00, 0x00, 0x06, 0x0E },
    { 0x00, 0x06, 0x0D, 0x00, 0x00, 0x07, 0x08 },
    { 0x00, 0x07, 0x06, 0x00, 0x00, 0x08, 0x02 },
    { 0x00, 0x07, 0x0F, 0x00, 0x00, 0x08, 0x0C },
    { 0x00, 0x09, 0x01, 0x00, 0x00, 0x0A, 0x00 },
    { 0x00, 0x09, 0x0A, 0x00, 0x00, 0x0A, 0x0A },
    { 0x00, 0x0B, 0x05, 0x00, 0x00, 0x0C, 0x08 },
    { 0x00, 0x0B, 0x0E, 0x00, 0x00, 0x0D, 0x02 },
    { 0x00, 0x0C, 0x07, 0x00, 0x00, 0x0D, 0x0C },
    { 0x00, 0x0D, 0x00, 0x00, 0x00, 0x0E, 0x06 },
    { 0x00, 0x0D, 0x08, 0x00, 0x00, 0x0E, 0x0F },
    { 0x00, 0x0E, 0x02, 0x00, 0x00, 0x0F, 0x0A },
    { 0x00, 0x0E, 0x0B, 0x00, 0x01, 0x00, 0x04 },
    { 0x00, 0x0F, 0x04, 0x00, 0x01, 0x00, 0x0E },
    { 0x01, 0x00, 0x06, 0x00, 0x01, 0x02, 0x02 },
    { 0x01, 0x02, 0x0A, 0x00, 0x01, 0x04, 0x0A },
    { 0x01, 0x03, 0x03, 0x00, 0x01, 0x05, 0x04 },
    { 0x01, 0x04, 0x05, 0x00, 0x01, 0x06, 0x08 },
    { 0x01, 0x04, 0x0E, 0x00, 0x01, 0x07, 0x02 },
    { 0x01, 0x05, 0x07, 0x00, 0x01, 0x07, 0x0C },
    { 0x01, 0x06, 0x00, 0x00, 0x01, 0x08, 0x06 },
    { 0x01, 0x06, 0x09, 0x00, 0x01, 0x09, 0x00 },
    { 0x01, 0x07, 0x02, 0x00, 0x01, 0x09, 0x0A }
  };
  write_midi((seg_t) { .n = 8,
      .s = (uint8_t [8]) { 0xF0, 0x47, 0x7F, 0x15, 0x5D, 0x00, 0x20, 0x00 }});
  write_midi((seg_t) { .n = sizeof(pad_thresh[0]), .s = pad_thresh[x] });
  write_midi((seg_t) { .n = 25, .s = (uint8_t[25]) {
        0x00, 0x00, 0x00, 0x02, 0x02, 0x02, 0x0E, 0x00,
        0x00, 0x00, 0x00, 0x01, 0x0D, 0x04, 0x0C, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7
      }});
}

void set_pad_curve(int x) {
  x = clamp(0, 5, x);
  static uint8_t pad_curve[6][20] = {
    { 0x01, 0x08, 0x06, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x0F, 0x0C, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x01, 0x04, 0x0C, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x0D, 0x04, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0E, 0x0A, 0x06 },
    { 0x01, 0x04, 0x0C, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x0D, 0x04, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x03, 0x05 },
    { 0x01, 0x08, 0x06, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x01, 0x0D, 0x04, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x03, 0x05 },
    { 0x01, 0x0F, 0x0B, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x01, 0x0D, 0x04, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x03, 0x05 },
    { 0x02, 0x02, 0x02, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x01, 0x0D, 0x04, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
  };
  write_midi((seg_t) { .n = 18,
      .s = (uint8_t [18]) {
        0xF0, 0x47, 0x7F, 0x15, 0x5D, 0x00, 0x20, 0x00,
        0x01, 0x07, 0x02, 0x00, 0x01, 0x09, 0x0A, 0x00, 0x00, 0x00 }});
  write_midi((seg_t) { .n = sizeof(pad_curve[0]), .s = pad_curve[x] });
  write_midi((seg_t) { .n = 2, .s = (uint8_t [2]) { 0x00, 0xf7 }});
}

static midi_tasks_state_t midi_state = DTASK_STATE(midi_tasks, 0, 0);

STATIC_ALLOC(midi_input_buffer, char, 128);
STATIC_ALLOC_DEPENDENT(push_rb, char, sizeof(ring_buffer_t) + static_sizeof(midi_input_buffer));
STATIC_ALLOC_DEPENDENT(synth_rb, char, sizeof(ring_buffer_t) + static_sizeof(midi_input_buffer));
STATIC_ALLOC_DEPENDENT(ext_rb, char, sizeof(ring_buffer_t) + static_sizeof(midi_input_buffer));

int fixed_length(unsigned char c) {
  int
    l = (c >> 4) & 7, // left 3 bits
    r = (c & 0x0f); // right 4 bits

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

seg_t find_midi_msg(unsigned char *status, const char **s, const char *e) {
  const char *p = *s;
  if(!*status) {
    // skip to first control byte
    while(p < e &&
          !((unsigned char)*p & 0x80)) {
      printf("* 0x%x\n", *p);
      p++;
    }
  }
  if(p == e) {
    *s = e;
    return (seg_t) {0};
  }
  if((unsigned char)*p & 0x80) {
    *status = *p++;
  }
  seg_t msg = { .s = p, .n = 0 };
  int len = fixed_length(*status);
  if(len < 0) { // sysex
    while(p < e) {
      msg.n++;
      if((unsigned char)*p == 0xf7) {
        *s = p + 1;
        return msg;
      }
      p++;
    }
    return (seg_t) {0};
  } else {
    msg.n = len - 1;
  }

  if(e - p < msg.n) {
    return (seg_t) {0};
  }

  *s = p + msg.n;
  return msg;
}

static
bool read_midi_msgs(struct midi *m, midi_tasks_state_t *state, dtask_set_t *events) {
  bool success = true;
  while(success) {

    // read from ring buffer first
    char *buffer = midi_input_buffer + 1;
    size_t remaining = static_sizeof(midi_input_buffer) - 1;
    ssize_t n = rb_read(m->rb, buffer, remaining);

    // fill remainder from input
    ssize_t r = snd_rawmidi_read(m->in, buffer + n, remaining - n);
    if(r < 0) {
      success = r == -EAGAIN;
      break;
    } else {
#if DEBUG
      printf("read %d >", m->id);
      COUNTUP(i, r) printf(" %02x", buffer[n + i]);
      printf("\n");
#endif
      n += r;
    }

    const char *buffer_end = buffer + n;
    seg_t msg;
    while(msg = find_midi_msg(&m->last_status, &buffer, buffer_end), msg.s) {
#if DEBUG
      printf("msg(%d, %02x)", m->id, m->last_status);
      COUNTUP(i, msg.n) printf(" %02x", msg.s[i]);
      printf("\n");
#endif
      midi_state.midi_in.status = m->last_status;
      if(!msg.n) m->last_status = 0; // ***
      midi_state.midi_in.id = m->id;
      midi_state.midi_in.data = msg;
      *events |= dtask_run((dtask_state_t *)state, MIDI_IN);
      if(midi_state.poweroff) {
        success = false;
        break;
      }
    }

    // push remaining bytes into the ring buffer
    n = rb_write(m->rb, buffer, buffer_end - buffer);
    success &= n == buffer_end - buffer;
  }
  return success;
}

void write_midi(seg_t s) {
  if(!_write_midi_file.active) {
    snd_rawmidi_write(push.out, s.s, s.n);
  }
}

void write_synth(seg_t s) {
#if DEBUG
  printf("synth: 0x%x:", (unsigned char)s.s[0]);
  RANGEUP(i, 1, s.n) {
    printf(" %d", (unsigned char)s.s[i]);
  }
  printf("\n");
#endif
  if(_write_midi_file.active) {
    write_midi_file_event(s);
  } else {
    while(s.n) {
      ssize_t n = snd_rawmidi_write(synth.out, s.s, s.n);
      assert_throw(n >= 0, "write_synth: write error %d\n", n);
      s.s += n;
      s.n -= n;
    }
  }
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
#if DEBUG
  printf("synth %d %s %d\n", (int)note, on ? "on" : "off", (int)pressure);
#endif
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

//                                      C  #  D  #  E  F  #  G  #   A  #  B
static const uint8_t background[12] = {45, 0, 1, 0, 3, 1, 0, 3, 0, 96, 0, 1};
static const char note_name[24] = "C C#D D#E F F#G G#A A#B ";

char *get_note_name(unsigned int note) {
  return &note_name[2 * (note % 12)];
}

int get_note_octave(unsigned int note) {
  return note / 12;
}

uint8_t background_color(unsigned int note, unsigned int scale) {
  return background[(12 + note - scale) % 12];
}

bool in_key(unsigned int scale, unsigned int key) {
  return !!background[(12 - scale + key) % 12];
}

unsigned int pad_to_note(unsigned int pad) {
  int
    x = pad & 7,
    y = pad >> 3;
  return
    x + y * 3.5; // shift rows by alternating 3/4 semitones
}

void write_text(int x, int y, seg_t s) {
  write_midi((seg_t) {
    .n = 8,
    .s = (char[8]) {0xf0, 0x47, 0x7f, 0x15, 0x18 + y, 0, s.n + 1, x}
  });
  write_midi(s);
  write_midi((seg_t) { .n = 1, .s = (char[1]) {0xf7} });
}

void printf_text(int x, int y, const char *fmt, ...) {
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
  int count = snd_rawmidi_poll_descriptors_count(push.in);
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

static
bool load_state(const char *name, midi_tasks_state_t *state) {
  int fd = open(name, O_RDONLY);
  if(fd >= 0) {
    char *task_specific = (char *)state + sizeof(dtask_state_t);
    size_t task_specific_size = sizeof(*state) - sizeof(dtask_state_t);
    read(fd, task_specific, task_specific_size);
    read(fd, record, static_sizeof(record)); // ***
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, task_specific, task_specific_size);
    crc = crc32(crc, record, static_sizeof(record));
    state->record.events = record;
    uLong crc_read;
    read(fd, &crc_read, sizeof(crc_read));
    close(fd);
    if(crc == crc_read) {
      printf("state loaded from: %s\n", name);
      return true;
    } else {
      printf("bad crc: %s\n", name);
      memset(task_specific, 0, task_specific_size);
      memset(record, 0, static_sizeof(record));
    }
  } else {
    printf("failed to load: %s\n", name);
  }
  return false;
}

static
void save_state(const char *name, const midi_tasks_state_t *state) {
  int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if(fd >= 0) {
    char *task_specific = (char *)state + sizeof(dtask_state_t);
    size_t task_specific_size = sizeof(*state) - sizeof(dtask_state_t);
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const Bytef *)task_specific, task_specific_size);
    crc = crc32(crc, (const Bytef *)record, static_sizeof(record));
    write(fd, task_specific, task_specific_size);
    write(fd, record, static_sizeof(record)); // ***
    write(fd, &crc, sizeof(crc));
    close(fd);
    printf("state saved to: %s\n", name);
  } else {
    printf("failed to save: %s\n", name);
  }
}

#define WRITE_BYTES(fd, ...) write(fd, (unsigned char[]) { __VA_ARGS__ }, sizeof((unsigned char[]) { __VA_ARGS__ }))

TEST(write_midi_file) {
  printf("sizeof bytes = %d\n", sizeof((unsigned char[]) {1, 2, 3}));
  return 0;
}

static
int write_var_length(int fd, unsigned int val) {
  bool significant = false;
  int n = 0;
  COUNTDOWN(i, DIV_UP(sizeof(val) * 8, 7)) {
    unsigned char byte = (val >> (7 * i)) & 0x7f;
    if(byte || !i) significant = true;
    if(significant) {
      if(i) byte |= 0x80;
      if(write(fd, &byte, 1) == -1) return -1;
      n++;
    }
  }
  return n;
}

static
int write_midi_file(const char *path, const midi_tasks_state_t *state) {
  static midi_tasks_state_t midi_state;
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if(fd < 0) return -1;
  memcpy(&midi_state, state, sizeof(midi_state)); // don't change input
  WRITE_BYTES(fd, 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 1, 0, BEATS_PER_PAGE); // Header
  WRITE_BYTES(fd, 'M', 'T', 'r', 'k', 0, 0, 0, 0); // Track header (seek back and write length)
  _write_midi_file = (struct write_midi_file) {
    .track_size = 0,
    .fd = fd,
    .last_beat = 0,
    .current_beat = 0,
    .active = true
  };
  midi_state.beat.then = 0;
  midi_state.beat.now = 0;
  midi_state.playing = true;

  // tempo
  unsigned int tempo = 60000000 / midi_state.bpm;
  write_midi_file_event((seg_t) {
      .n = 6,
      .s = (char [6]) { 0xff, 0x51, 0x03, tempo >> 16, tempo >> 8, tempo }
    });

  // set up tracks
  dtask_switch((dtask_state_t *)&midi_state, PLAYBACK | PLAYING);
  dtask_run((dtask_state_t *)&midi_state, PLAYING);

  // playback to file
  COUNTUP(beat, BEATS) {
    _write_midi_file.current_beat = beat;
    dtask_run((dtask_state_t *)&midi_state, EXTERNAL_TICK);
  }

  // end track
  write_midi_file_event((seg_t) {
      .n = 3,
      .s = (char [3]) { 0xff, 0x2f, 0x00 }
    });

  // seek to byte 18 and write track size
  lseek(fd, 18, SEEK_SET);
  uint32_t size = htobe32(_write_midi_file.track_size);
  write(fd, &size, 4);
  close(fd);
  _write_midi_file.active = false;
  _write_midi_file.fd = -1;
  return 0;
}

unsigned int mod_diff(unsigned int a, unsigned int b, unsigned int m) {
  return (m + a - b) % m;
}

void write_midi_file_event(seg_t s) {
  ssize_t n;
  // write delta time
  n = write_var_length(_write_midi_file.fd,
                       mod_diff(_write_midi_file.current_beat,
                                _write_midi_file.last_beat,
                                BEATS));
  assert_throw(n >= 0);
  _write_midi_file.track_size += n;
  _write_midi_file.last_beat = _write_midi_file.current_beat;

  // write the MIDI event
  n = write(_write_midi_file.fd, s.s, s.n);
  assert_throw(n >= 0);
  _write_midi_file.track_size += n;
}

static
void save(const midi_tasks_state_t *state) {
  struct tm *date = localtime(&state->time_of_day.tv_sec);
  char filename[64];
  snprintf(filename, sizeof(filename),
           "record/%04d%02d%02d-%02d%02d%02d.state",
           date->tm_year + 1900, date->tm_mon + 1, date->tm_mday,
           date->tm_hour, date->tm_min, date->tm_sec);
  printf("save state: %s\n", filename);
  save_state(filename, state);
  snprintf(filename, sizeof(filename),
           "record/%04d%02d%02d-%02d%02d%02d.mid",
           date->tm_year + 1900, date->tm_mon + 1, date->tm_mday,
           date->tm_hour, date->tm_min, date->tm_sec);
  write_midi_file(filename, state);
  printf("save MIDI: %s\n", filename);
}

STATIC_ALLOC(record, pair_t, 1 << 15);
int main(int argc, char *argv[]) {
  static_alloc_init();
  log_init();

  error_t test_error;
  CATCH(&test_error, true) {
    printf(NOTE("ERROR") " ");
    print_last_log_msg();
    return -1;
  } else {

    // run tests if requested
    if(argc >= 3 && strcmp(argv[1], "-t") == 0) {
      run_test(string_seg(argv[2]));
      return 0;
    }

    // get parameters
    int curve = 1, threshold = 15;
    if(argc >= 3) {
      curve = strtol(argv[1], NULL, 0);
      threshold = strtol(argv[2], NULL, 0);
    }

    // open devices
    int push_card = find_card("Ableton Push");
    assert_throw(push_card >= 0, "Ableton Push not found.");
    int virtual_card = find_card("VirMIDI");
    assert_throw(virtual_card >= 0, "VirMIDI not found.");

    midi_open(&push,  0, push_card,    0, push_rb,  static_sizeof(push_rb));
    midi_open(&synth, 1, virtual_card, 0, synth_rb, static_sizeof(synth_rb));
    midi_open(&ext,   2, virtual_card, 1, ext_rb,   static_sizeof(ext_rb));

    // initialize Push
    COUNTUP(i, sizeof(push_init)) {
      char c = push_init[i];
      int n = snd_rawmidi_write(push.out, &c, 1);
      assert_throw(n >= 0, "Problem sending initialization: %s", snd_strerror(n));
      if((unsigned char)c & 0x80) usleep(1000);
    }
    set_pad_curve(curve);
    set_pad_threshold(threshold);

    // load state
    if(!load_state(STATE_FILE, &midi_state)) {
      midi_state.record.events = init_map(record, record_size);
    }

    // collect poll fds
    pfds_in_n = get_pfds(push.in, pfds_in, LENGTH(pfds_in));
    pfds_in_n += get_pfds(synth.in, pfds_in + pfds_in_n, LENGTH(pfds_in) - pfds_in_n);
    pfds_in_n += get_pfds(ext.in, pfds_in + pfds_in_n, LENGTH(pfds_in) - pfds_in_n);

    // enable and select tasks
    dtask_enable((dtask_state_t *)&midi_state, initial);
    dtask_select((dtask_state_t *)&midi_state);

    // event loop
    dtask_set_t events = 0;
    while(read_midi_msgs(&push,  &midi_state, &events) &&
          read_midi_msgs(&synth, &midi_state, &events) &&
          read_midi_msgs(&ext,   &midi_state, &events)) {
      poll(pfds_in, pfds_in_n, 10);
      gettimeofday(&midi_state.time_of_day, NULL);
      events |= dtask_run((dtask_state_t *)&midi_state, TIME_OF_DAY);
      if(events & SAVE) {
        save(&midi_state);
      }
      events = 0;
    }

    // disable tasks, save state, and close
    dtask_disable((dtask_state_t *)&midi_state, initial);
    save_state(STATE_FILE, &midi_state);
    write_midi_file(MIDI_FILE, &midi_state);

    midi_close(&push);
    midi_close(&synth);
    midi_close(&ext);
    return midi_state.poweroff ? 40 : 0;
  }
}

static
void vec128b_print(vec128b *v) {
  printf("v =");
  COUNTDOWN(i, LENGTH(v->word)) {
    printf(" %0.*x", sizeof(v->word[0]) * 2, v->word[i]);
  }
  printf("\n");
}

TEST(vec128b_shift) {
  vec128b v;
  FOREACH(i, v.word) {
    v.word[i] = i + 1;
  }
  vec128b_print(&v);

  printf("shift left ________________\n");
  COUNTUP(i, 128 / 4) {
    vec128b_shiftl(&v, 4);
    vec128b_print(&v);
  }
  FOREACH(i, v.word) {
    v.word[i] = i + 1;
  }
  COUNTUP(i, 128 / 32) {
    vec128b_shiftl(&v, 32);
    vec128b_print(&v);
  }
  printf("shift right ________________\n");
  FOREACH(i, v.word) {
    v.word[i] = i + 1;
  }
  COUNTUP(i, 128 / 4) {
    vec128b_shiftr(&v, 4);
    vec128b_print(&v);
  }
  FOREACH(i, v.word) {
    v.word[i] = i + 1;
  }
  COUNTUP(i, 128 / 32) {
    vec128b_shiftr(&v, 32);
    vec128b_print(&v);
  }
  return 0;
}
