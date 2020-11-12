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

#ifndef DTASK_GEN
#include "types.h"
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "startle/types.h"
#include "startle/macros.h"
#include "startle/map.h"
#include "midipush.h"
#include "midi_tasks.h"
#endif

#define BEATS_PER_PAGE 24
#define PAGES 64
#define BEATS (BEATS_PER_PAGE * PAGES)

#define PAD_RED 5
#define PAD_YELLOW 13
#define PAD_GREEN 21

DTASK_GROUP(midi_tasks)

// passthrough
DTASK(push_midi_msg_in, seg_t) {
  return true;
}

DTASK(msg_count, unsigned int) {
  return true;
}

DTASK(time_of_day, struct timeval) {
  return true;
}

DTASK(tick, long long) {
  timeval_t tv = *DREF(time_of_day);
  long long ms = tv.tv_sec * 1000ll + tv.tv_usec / 1000ll;
  if(ms < *DREF(tick) ||
     ms - *DREF(tick) > 60000 / (*DREF(bpm) * BEATS_PER_PAGE)) {
    *DREF(tick) = ms;
    return true;
  } else {
    return false;
  }
}

DTASK(beat, struct { unsigned int then, now; }) {
  DREF(beat)->then = DREF(beat)->now;
  if((state->events & TICK) && *DREF(playing)) {
    (void)*DREF(tick);
    DREF(beat)->now = (DREF(beat)->then + 1) % BEATS;
  }
  if(state->events & SHUTTLE) {
    DREF(beat)->now = (DREF(beat)->then + *DREF(shuttle) * BEATS_PER_PAGE / 4) % BEATS;
  }
  if(state->events & SET_PAGE) {
    unsigned int current_page = DREF(beat)->then / BEATS_PER_PAGE;
    DREF(beat)->now = ((current_page & DREF(set_page)->keep) | DREF(set_page)->val) * BEATS_PER_PAGE;
  }
  return true;
}

DTASK(print_midi_msg, bool) {
  const seg_t *msg = DREF(push_midi_msg_in);
  printf("0x%x:", (unsigned char)msg->s[0]);
  RANGEUP(i, 1, msg->n) {
    printf(" %d", (unsigned char)msg->s[i]);
  }
  printf("\n");
  return true;
}

void set_bit(uint64_t *s, int k, bool v) {
  if(v) {
    *s |= 1ull << k;
  } else {
    *s &= ~(1ull << k);
  }
}

DTASK(pad, uint64_t) {
  const seg_t *msg_in = DREF(push_midi_msg_in);
  unsigned char control = msg_in->s[0] & 0xf0;
  if(ONEOF(control, 0x80, 0x90)) {
    int p = msg_in->s[1] - 36;
    if(INRANGE(p, 0, 63)) {
      set_bit(DREF(pad), p, control == 0x90);
      return true;
    }
  }
  return false;
}

DTASK(notes, uint64_t) {
  const seg_t *msg_in = DREF(push_midi_msg_in);
  unsigned char control = msg_in->s[0] & 0xf0;
  if(ONEOF(control, 0x80, 0x90)) {
    int p = msg_in->s[1] - 36;
    if(INRANGE(p, 0, 63)) {
      set_bit(DREF(notes), pad_to_note(p), control == 0x90);
      return true;
    }
  }
  return false;
}

DTASK_ENABLE(deleting) {
  send_msg(0xb0, 118, 1);
  *DREF(deleting) = false;
}

DTASK(deleting, bool) {
  const seg_t *msg_in = DREF(push_midi_msg_in);
  unsigned char control = msg_in->s[0] & 0xf0;
  if(control == 0xb0 && msg_in->s[1] == 118 && msg_in->s[2]) {
    *DREF(deleting) = !*DREF(deleting);
    send_msg(0xb0, 118, *DREF(deleting) ? 2 : 1);
    return true;
  }
  return false;
}

DTASK_ENABLE(bpm) {
  *DREF(bpm) = 60;
  printf_text(0, 3, "bpm: %3d", *DREF(bpm));
}

DTASK(bpm, int) {
  const seg_t *msg_in = DREF(push_midi_msg_in);
  unsigned char control = msg_in->s[0] & 0xf0;
  if(control == 0xb0 && msg_in->s[1] == 14) {
    int val = msg_in->s[2];
    if(val >= 64) val = val - 128;
    *DREF(bpm) = clamp(30, 240, *DREF(bpm) + val * 5);
    printf_text(0, 3, "bpm: %3d", *DREF(bpm));
    return true;
  }
  return false;
}

typedef union {
  char byte[sizeof(uintptr_t)];
  uintptr_t data;
} msg_data_t;

DTASK(note_record, struct { uint64_t notes[BEATS][16]; }) {
  int channel = *DREF_PASS(channel);
  if(*DREF(new_button)) {
    if(!*DREF_PASS(deleting)) {
      memset(DREF(note_record), 0, sizeof(note_record_t));
    } else {
      COUNTUP(i, BEATS) {
        DREF(note_record)->notes[i][channel] = 0;
      }
    }
  } else {
    int start = DREF(beat)->then, stop = DREF(beat)->now;
    if((stop - start + BEATS) % BEATS > BEATS / 2) {
      int tmp = start;
      start = stop;
      stop = tmp;
    }
    if(*DREF(deleting)) {
      int i = start;
      while(i != stop) {
        DREF(note_record)->notes[i][channel] &= ~*DREF(notes);
        i = (i + 1) % BEATS;
      }
    } else if(*DREF(recording)) {
      int i = start;
      while(i != stop) {
        DREF(note_record)->notes[i][channel] |= *DREF(notes);
        i = (i + 1) % BEATS;
      }
    }
  }
  return true;
}

DTASK(record, map_t) {
  unsigned int beat = DREF_PASS(beat)->then;
  int channel = *DREF_PASS(channel);
  map_t record = *DREF(record);
  if(*DREF(new_button)) {
    if(!*DREF(deleting)) {
      map_clear(record);
    } else {
      FORMAP(i, record) {
        pair_t *p = &record[i];
        msg_data_t msg = { .data = p->second };
        if((msg.byte[0] & 0xf) == channel) {
          p->second = 0;
        }
      }
    }
    return true;
  }
  if(*DREF(deleting)) {
    map_iterator it = map_iterator_begin(record, beat);
    pair_t *p = map_find_iter(&it);
    while(p) {
      msg_data_t msg = { .data = p->second };
      if((msg.byte[0] & 0xf) == channel) {
        p->second = 0;
      }
      p = map_next(&it, p);
    }
  } else {
    if(state->events & DELETING) {
      // delete disabled, so collect garbage
      int n = map_filter(record, nonzero_value);
      printf("%d notes discarded\n", n);
    }
    if(*DREF(recording) &&
       state->events & PUSH_MIDI_MSG_IN) {
      const seg_t *msg_in = DREF(push_midi_msg_in);
      unsigned char control = msg_in->s[0] & 0xf0;
      if(ONEOF(control, 0x80, 0x90)) {
        int pad = msg_in->s[1] - 36;
        if(INRANGE(pad, 0, 63)) {
          int note = pad_to_note(pad);
          if(control == 0x90) {
            msg_data_t msg = {{
                msg_in->s[0] | channel,
                note,
                msg_in->s[2]
              }};
            map_insert(record, PAIR(beat, msg.data));
          }
          return true;
        }
      }
    }
  }
  return false;
}

DTASK(passthrough, bool) {
  const seg_t *msg_in = DREF(push_midi_msg_in);
  int channel = *DREF_PASS(channel);
  unsigned char control = msg_in->s[0] & 0xf0;
  if(ONEOF(control, 0x80, 0x90)) {
    int pad = msg_in->s[1] - 36;
    if(INRANGE(pad, 0, 63)) {
      synth_note(channel, pad_to_note(pad) + DREF_PASS(transpose)->offset[channel], control == 0x90, msg_in->s[2]);
      return true;
    }
  }
  return false;
}

DTASK(set_page, struct { int val, set, keep; }) {
  if(*DREF(new_button)) {
    *DREF(set_page) = (set_page_t) {
      .val = 0,
      .set = 0,
      .keep = 0
    };
    return true;
  }
  const seg_t *msg_in = DREF(push_midi_msg_in);
  unsigned char control = msg_in->s[0] & 0xf0;
  if(control == 0xb0) {
    int c = msg_in->s[1];
    bool top = INRANGE(c, 20, 27);
    bool bottom = INRANGE(c, 102, 109);
    set_page_t *p = DREF(set_page);
    if(msg_in->s[2]) {
      if(bottom) {
        p->val = (c - 102) * 8 | (p->val & 0x07);
        p->set |= 0x38;
      } else if(top) {
        p->val = (p->val & 0x38) | (c - 20);
        p->set |= 0x07;
      }
    } else if (p->set) {
      p->val &= p->set;
      p->keep = 0x38 & ~p->set; // subpage = 0 if not set
      p->set = 0;
      return top || bottom; // event on release
    }
  }
  return false;
}

DTASK_ENABLE(playback) {
  COUNTUP(i, 64) {
    set_pad_color(i, background_color(pad_to_note(i)));
  }
}

uint64_t notes_to_pads(uint64_t notes) {
  uint64_t pads = 0;
  COUNTUP(i, 64) {
    int note = pad_to_note(i);
    if(notes & (1ull << notes)) {
      pads |= 1ull << i;
    }
  }
  return pads;
}

DTASK(playback, uint64_t) {
  uint64_t prev = *DREF(playback);
  unsigned int beat = DREF(beat)->now;
  uint64_t *notes = DREF(note_record)->notes[beat];
  uint64_t *prev_notes = DREF(note_record)->notes[DREF(beat)->then];
  int channel = *DREF_PASS(channel);
  unsigned int disable = *DREF_PASS(disable_channel);

  if(*DREF(playing)) {
    map_iterator it = map_iterator_begin(*DREF(record), beat);
    pair_t *p = map_find_iter(&it);
    while(p) {
      msg_data_t msg = { .data = p->second };
      if((msg.byte[0] & 0xf0) == 0x90) {
        int c = msg.byte[0] & 0x0f;
        if(!(disable & 1ull << c)) {
          synth_note(c, msg.byte[1] + DREF_PASS(transpose)->offset[c], true, msg.byte[2]);
        }
      }
      p = map_next(&it, p);
    }
    COUNTUP(c, 16) {
      if(disable & 1ull << c) {
        continue;
      }
      uint64_t pressed = notes[c] | (c == channel ? *DREF(notes) : 0);
      uint64_t released = prev_notes[c] & ~pressed;
      COUNTUP(i, 64) {
        uint64_t bit = 1ull << i;
        if(released & bit) {
          synth_note(c, i + DREF_PASS(transpose)->offset[c], false, 0);
        }
      }
    }
  }

  uint64_t cur = *DREF(pad);
  bool first = true;

  COUNTUP(i, 64) {
    uint64_t bit = 1ull << i;
    int note = pad_to_note(i);
    uint64_t note_bit = 1ull << note;
    int color = 0;
    if(notes[channel] & note_bit) {
      cur |= bit;
    }
    if((~prev & cur) & bit) { // set
      if(*DREF(pad) & bit) {
        if(first) { // show first pressed note
          printf_text(0, 0, "note: %.2s, octave: %2d, number: %2d",
                      get_note_name(note),
                      get_note_octave(note) + (DREF_PASS(transpose)->offset[channel] - 7) / 12, // TODO simplify this
                      note);
          first = false;
        }
        color = PAD_GREEN;
      } else {
        color = PAD_RED;
      }
    } else if((prev & ~cur) & bit) { // cleared
      color = background_color(note);
    } else {
      continue;
    }
    set_pad_color(i, color);
  }
  *DREF(playback) = cur;
  return true;
}

DTASK(light_trail, bool) {
  const seg_t *msg_in = DREF(push_midi_msg_in);
  if((unsigned char)msg_in->s[0] == 0xd0) {
    unsigned int x = msg_in->s[1];
    unsigned int rgb = (x << 17) | (x << 9) | (x << 1);
    set_pad_rgb_color(rand() & 0x3f, rgb);
    return true;
  }
  return false;
}

DTASK_ENABLE(light_bar) {
  COUNTUP(i, 8) {
    send_msg(0xb0, i + 20, 0);
    send_msg(0xb0, i + 102, 0);
  }
}

DTASK(light_bar, int) {
  int new = DREF(beat)->now / BEATS_PER_PAGE;
  int old = *DREF_WEAK(light_bar);
  int diff = new ^ old;
  if(diff & 0x07) send_msg(0xb0, (old & 7) + 20, 0);
  if(diff & 0x38) send_msg(0xb0, ((old >> 3) & 7) + 102, 0);
  send_msg(0xb0, (new & 7) + 20, 22);
  send_msg(0xb0, ((new >> 3) & 7) + 102, 22);
  *DREF(light_bar) = new;
  return true;
}

DTASK_ENABLE(new_button) {
  send_msg(0xb0, 87, 1);
}

DTASK(new_button, bool) {
  const seg_t *msg_in = DREF(push_midi_msg_in);
  unsigned char control = msg_in->s[0] & 0xf0;
  if(control == 0xb0 && msg_in->s[1] == 87) {
    *DREF(new_button) = msg_in->s[2];
    if(msg_in->s[2]) COUNTUP(c, 16) all_notes_off(c);
    return true;
  }
  return false;
}

DTASK_ENABLE(playing) {
  send_msg(0xb0, 85, 2);
  *DREF(playing) = false;
}

DTASK(playing, bool) {
  const seg_t *msg_in = DREF(push_midi_msg_in);
  unsigned char control = msg_in->s[0] & 0xf0;
  if(control == 0xb0 && msg_in->s[1] == 85 && msg_in->s[2]) {
    *DREF(playing) = !*DREF(playing);
    send_msg(0xb0, 85, *DREF(playing) ? 1 : 2);
    if(*DREF(playing)) { // send programs on play
      COUNTUP(channel, 16) {
        uint8_t *p = &DREF_PASS(program)->arr[channel];
        write_synth((seg_t) {
          .n = 2,
          .s = (char [2]) { 0xc0 | channel, *p }
        });
      }
    } else {
      COUNTUP(c, 16) all_notes_off(c);
    }
    return true;
  }
  return false;
}

DTASK_ENABLE(recording) {
  send_msg(0xb0, 86, 0);
  *DREF(recording) = false;
}

DTASK(recording, bool) {
  const seg_t *msg_in = DREF(push_midi_msg_in);
  unsigned char control = msg_in->s[0] & 0xf0;
  if(control == 0xb0 && msg_in->s[1] == 86 && msg_in->s[2]) {
    *DREF(recording) = !*DREF(recording);
    send_msg(0xb0, 86, *DREF(recording) ? 4 : 0);
    return true;
  }
  return false;
}

DTASK(shuttle, int8_t) {
  const seg_t *msg_in = DREF(push_midi_msg_in);
  unsigned char control = msg_in->s[0] & 0xf0;
  if(control == 0xb0 && msg_in->s[1] == 15 && msg_in->s[2]) {
    int8_t val = msg_in->s[2];
    if(val >= 64) val = val - 128;
    *DREF(shuttle) = val;
    return true;
  }
  return false;
}

DTASK(program, struct { int arr[16]; }) {
  const seg_t *msg_in = DREF(push_midi_msg_in);
  int channel = *DREF_PASS(channel);
  bool change = false;
  unsigned char control = msg_in->s[0] & 0xf0;
  if(control == 0xb0 &&
       msg_in->s[1] == 79) {
    uint8_t val = msg_in->s[2];
    uint8_t *p = &DREF(program)->arr[channel];
    *p = (*p + val) % 128;
    write_synth((seg_t) {
      .n = 2,
      .s = (char [2]) { 0xc0 | channel, *p }
    });
    return true;
  }
  return false;
}

DTASK_ENABLE(show_program) {
  printf_text(51, 0, "program: %3d", DREF(program)->arr[*DREF(channel)]);
}

DTASK(show_program, bool) {
  printf_text(51, 0, "program: %3d", DREF(program)->arr[*DREF(channel)]);
  return true;
}

DTASK_ENABLE(channel) {
  send_msg(0xb0, 46, 1);
  send_msg(0xb0, 47, 1);
  *DREF(channel) = 0;
  printf_text(51, 1, "channel: %2d", *DREF(channel));
}

DTASK(channel, int) {
  const seg_t *msg_in = DREF(push_midi_msg_in);
  unsigned char control = msg_in->s[0] & 0xf0;
  if(control == 0xb0 &&
     ONEOF(msg_in->s[1], 46, 47) &&
     msg_in->s[2]) {
    *DREF(channel) = (*DREF(channel) + (msg_in->s[1] == 47 ? 15 : 1)) % 16;
    printf_text(51, 1, "channel: %2d", *DREF(channel));
    return true;
  }
  return false;
}

DTASK_ENABLE(show_disable_channel) {
  send_msg(0xb0, 48, 4);
  *DREF(disable_channel) = 0;
}

DTASK(show_disable_channel, bool) {
  send_msg(0xb0, 48, *DREF(disable_channel) & (1ull << *DREF(channel)) ? 0 : 4);
  return true;
}

DTASK(disable_channel, unsigned int) {
  int channel = *DREF_PASS(channel);
  if(state->events & PUSH_MIDI_MSG_IN) {
    const seg_t *msg_in = DREF(push_midi_msg_in);
    unsigned char control = msg_in->s[0] & 0xf0;
    if(control == 0xb0 &&
       msg_in->s[1] == 48 &&
       msg_in->s[2]) {
      *DREF(disable_channel) ^= 1ull << channel;
      return true;
    }
  }
  return false;
}

DTASK_ENABLE(transpose) {
  send_msg(0xb0, 44, 1);
  send_msg(0xb0, 45, 1);
  COUNTUP(c, 16) {
    DREF(transpose)->offset[c] = 43;
  }
  printf_text(51, 2, "octave: %1d", (int)(DREF(transpose)->offset[*DREF(channel)] - 7) / 12);
}

DTASK(transpose, struct { uint8_t offset[16]; }) {
  const seg_t *msg_in = DREF(push_midi_msg_in);
  unsigned char control = msg_in->s[0] & 0xf0;
  if(control == 0xb0 &&
     ONEOF(msg_in->s[1], 44, 45) &&
     msg_in->s[2]) {
    int channel = *DREF_PASS(channel);
    uint8_t *off = &DREF(transpose)->offset[channel];
    *off = clamp(7, 67, *off + (msg_in->s[1] == 44 ? -12 : 12));
    printf_text(51, 2, "octave: %1d", (int)((*off - 7) / 12));
    all_notes_off(channel);
    return true;
  }
  return false;
}
