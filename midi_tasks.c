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
#include "vec128b.h"
#include "midi_tasks.h"
#endif

#define BEATS_PER_PAGE 24
#define PAGES 64
#define BEATS (BEATS_PER_PAGE * PAGES)
#define BANKS 26

#define PAD_RED 5
#define PAD_YELLOW 13
#define PAD_GREEN 21

static_assert(MIDI_TASKS_COUNT <= 64, "too many tasks");

DTASK_GROUP(midi_tasks)

// passthrough
DTASK(midi_in, struct { int id; seg_t msg; }) {
  return true;
}

DTASK(time_of_day, struct timeval) {
  return true;
}

DTASK(tick, long long) {
  timeval_t tv = *DREF(time_of_day);
  long long ms = tv.tv_sec * 1000ull + tv.tv_usec / 1000ull;
  if(ms < *DREF(tick) ||
     ms - *DREF(tick) > 60000 / (*DREF(bpm) * BEATS_PER_PAGE)) {
    *DREF(tick) = ms;
    return true;
  } else {
    return false;
  }
}

unsigned int page_beat(unsigned int b, const set_page_t *p) {
  unsigned int current_page = b / BEATS_PER_PAGE;
  return ((current_page & p->keep) | p->val) * BEATS_PER_PAGE;
}

DTASK(beat, struct { unsigned int then, now; }) {
  DREF(beat)->then = DREF(beat)->now;
  if((state->events & TICK) && *DREF(playing)) {
    (void)*DREF(tick);
    DREF(beat)->now = (DREF(beat)->then + 1) % BEATS;
  }
  if(state->events & SHUTTLE) {
    DREF(beat)->now = (DREF(beat)->then + BEATS + *DREF(shuttle) * BEATS_PER_PAGE / 4) % BEATS;
  }
  if((state->events & SET_PAGE) && DREF(set_page)->note == -1 && DREF(set_page)->keep != 0xff) {
    DREF(beat)->now = page_beat(DREF(beat)->then, DREF(set_page));
  }
  return true;
}

DTASK(print_midi_msg, bool) {
  const seg_t *msg = &DREF(midi_in)->msg;
  printf("%d > 0x%x:", DREF(midi_in)->id, (unsigned char)msg->s[0]);
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

DTASK(pad, struct { bool pressed; uint8_t id, velocity; }) {
  if(DREF(midi_in)->id != 0) return false;
  const seg_t *msg_in = &DREF(midi_in)->msg;
  unsigned char control = msg_in->s[0] & 0xf0;
  if(ONEOF(control, 0x80, 0x90)) {
    int p = msg_in->s[1] - 36;
    if(INRANGE(p, 0, 63)) {
      *DREF(pad) = (pad_t) {
        .pressed = control == 0x90,
        .id = p,
        .velocity = msg_in->s[2]
      };
      return true;
    }
  }
  return false;
}

DTASK(external_key, struct { bool pressed; uint8_t id, velocity; }) {
  if(DREF(midi_in)->id == 0) return false;
  const seg_t *msg_in = &DREF(midi_in)->msg;
  unsigned char control = msg_in->s[0] & 0xf0;
  if(ONEOF(control, 0x80, 0x90)) {
    *DREF(external_key) = (external_key_t) {
      .pressed = control == 0x90,
      .id = msg_in->s[1],
      .velocity = msg_in->s[2]
    };
    return true;
  }
  return false;
}

DTASK(channel_pressure, int) {
  if(DREF(midi_in)->id != 0) return false;
  const seg_t *msg_in = &DREF(midi_in)->msg;
  unsigned char control = msg_in->s[0] & 0xf0;
  if(control == 0xd0) {
    *DREF(channel_pressure) = msg_in->s[1];
    return true;
  }
  return false;
}

DTASK(pitch_bend, int) {
  if(DREF(midi_in)->id != 0) return false;
  const seg_t *msg_in = &DREF(midi_in)->msg;
  unsigned char control = msg_in->s[0] & 0xf0;
  if(control == 0xe0) {
    *DREF(pitch_bend) = (msg_in->s[1] & 0x7f) | (((int)msg_in->s[2] & 0x7f) << 7) ;
    return true;
  }
  return false;
}

DTASK(control_change, struct { int control, value; } ) {
  if(DREF(midi_in)->id != 0) return false;
  const seg_t *msg_in = &DREF(midi_in)->msg;
  unsigned char control = msg_in->s[0] & 0xf0;
  if(control == 0xb0) {
    DREF(control_change)->control = msg_in->s[1];
    DREF(control_change)->value = msg_in->s[2];
    return true;
  }
  return false;
}

DTASK(pads, uint64_t) {
  const pad_t *pad = DREF(pad);
  uint64_t prev = *DREF(pads);
  if(*DREF(new_button)) {
    *DREF(pads) = 0;
  } else {
    set_bit(DREF(pads), pad->id, pad->pressed);
  }
  return *DREF(pads) != prev;
}

DTASK(current_note, struct { bool on; uint8_t id, velocity; }) {
  if(state->events & PAD) {
    const pad_t *pad = DREF(pad);
    int id = pad_to_note(pad->id) + *DREF(transpose);
    if(INRANGE(id, 0, 127)) {
      *DREF(current_note) = (current_note_t) {
        .on = pad->pressed,
        .id = pad_to_note(pad->id) + *DREF(transpose),
        .velocity = pad->velocity
      };
      return true;
    }
  }
  if(state->events & EXTERNAL_KEY) {
    const pad_t *key = DREF(external_key);
    *DREF(current_note) = (current_note_t) {
      .on = key->pressed,
      .id = key->id,
      .velocity = key->velocity
    };
    return true;
  }
  return false;
}

DTASK(notes, vec128b) {
  (void)DREF(transpose);
  const current_note_t *note = DREF(current_note);
  vec128b prev = *DREF(notes);
  if(state->events & TRANSPOSE) {
    vec128b_set_zero(DREF(notes));
  } else {
    vec128b_set_bit_val(DREF(notes), note->id, note->on);
  }
  return !vec128b_eq(DREF(notes), &prev);
}

DTASK_ENABLE(deleting) {
  send_msg(0xb0, 118, 1);
  *DREF(deleting) = false;
}

DTASK(deleting, bool) {
  const control_change_t *cc = DREF(control_change);
  if(cc->control == 118 && cc->value) {
    *DREF(deleting) = !*DREF(deleting);
    send_msg(0xb0, 118, *DREF(deleting) ? 2 : 1);
    return true;
  }
  // automatically disable after new
  if((state->events & NEW_BUTTON) && !*DREF(new_button)) {
    *DREF(deleting) = false;
    send_msg(0xb0, 118, 1);
  }
  return false;
}

DTASK_ENABLE(bpm) {
  *DREF(bpm) = 60;
  printf_text(0, 3, "bpm: %3d", *DREF(bpm));
}

DTASK(bpm, int) {
  const control_change_t *cc = DREF(control_change);
  if(cc->control == 14) {
    int val = cc->value;
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

DTASK_ENABLE(record) {
  record_t *record = DREF(record);
  record->copy.shift = -1;
  record->copy.first_note = -1;
  record->copy.first_beat = -1;
}

// TODO this is too long
DTASK(record, struct { map_t events; vec128b notes[BEATS][16]; struct { int shift, first_beat, first_note; } copy; vec128b extra[16]; unsigned int active; }) {
  unsigned int beat = DREF_PASS(beat)->then;
  int channel = *DREF_PASS(channel);
  record_t *record = DREF(record);
  memset(record->extra, 0, sizeof(record->extra));

  if(state->events & SET_PAGE) {
    if(DREF(set_page)->note >= 0) {
      if(record->copy.first_beat < 0) {
        int start = page_beat(beat, DREF(set_page));
        COUNTUP(i, BEATS) {
          COUNTUP(c, 16) {
            if(!vec128b_zero(&record->notes[start][c])) {
              goto found_start;
            }
          }
          start = (start + 1) % BEATS;
        }
      found_start:
        record->copy.first_beat = start;
      }
      record->copy.shift = (beat + BEATS - record->copy.first_beat + 1) % BEATS;
    } else {
      record->copy.shift = -1;
      record->copy.first_note = -1;
      record->copy.first_beat = -1;
    }
    return true;
  }

  if(*DREF(new_button)) {
    if(*DREF(deleting)) {
      map_clear(record->events);
      memset(record->notes, 0, sizeof(record->notes));
      record->active = 0;
    } else {
      FORMAP(i, record->events) {
        pair_t *p = &record->events[i];
        msg_data_t msg = { .data = p->second };
        if((msg.byte[0] & 0xf) == channel) {
          p->second = 0;
        }
      }
      map_filter(record->events, nonzero_value);

      COUNTUP(i, BEATS) {
        vec128b_set_zero(&record->notes[i][channel]);
      }
      record->active &= ~(1 << channel);
    }
    return true;
  }

  int start = DREF(beat)->then, stop = DREF(beat)->now;
  if((stop - start + BEATS) % BEATS > BEATS / 2) {
    int tmp = start;
    start = stop;
    stop = tmp;
  }

  if(*DREF(deleting) &&
     !vec128b_zero(DREF(notes))) {

    // for clearing tails later
    vec128b clear = *DREF(notes);
    vec128b_and(&clear, &record->notes[(stop + BEATS - 1) % BEATS][channel]);

    for(int i = start; i != stop; i = (i + 1) % BEATS) {
      // notes
      vec128b_and_not(&record->notes[i][channel], DREF(notes));

      // events
      map_iterator it = map_iterator_begin(record->events, i);
      pair_t *p = map_find_iter(&it);
      while(p) {
        msg_data_t msg = { .data = p->second };
        if(msg.byte[0] == (0x90 | channel) &&
           vec128b_bit_is_set(DREF(notes), msg.byte[1])) {
          p->second = 0;
        }
        p = map_next(&it, p);
      }
    }

    // clear the tails from notes that have been deleted
    for(int i = stop; !vec128b_zero(&clear); i = (i + 1) % BEATS) {
      vec128b_and(&clear, &record->notes[i][channel]);
      vec128b_and_not(&record->notes[i][channel], &clear);
    }
    return true;
  }

  if(state->events & DELETING) {
    // delete disabled, so collect garbage
    map_filter(record->events, nonzero_value);
  }

  // copy
  if(record->copy.shift >= 0) {
    unsigned int disable = *DREF_PASS(disable_channel);
    bool change = false;
    // copy ahead one beat for playback
    start = (start + 1) % BEATS;
    stop = (stop + 1) % BEATS;
    for(int i = start; i != stop; i = (i + 1) % BEATS) {
      int src = (i + BEATS - record->copy.shift) % BEATS;

      // events
      map_iterator it = map_iterator_begin(record->events, src);
      pair_t *p = map_find_iter(&it);
      while(p) {
        msg_data_t msg = { .data = p->second };
        if(record->copy.first_note < 0) {
          if((msg.byte[0] & 0xf0) == 0x90) {
            record->copy.first_note = (int)msg.byte[1];
          }
        } else {
          int c = msg.byte[0] & 0x0f;
          if(!(disable & 1 << c)) {
            if((msg.byte[0] & 0xf0) == 0x90) {
              int transpose = DREF(set_page)->note - record->copy.first_note;
              int n = msg.byte[1] + transpose;
              if(INRANGE(n, 0, 127)) { // transpose notes
                msg.byte[1] = n;
                if(*DREF(recording) && !vec128b_bit_is_set(&record->notes[i][c], n)) {
                  map_insert(record->events, PAIR(i, msg.data));
                } else {
                  synth_note(c, n, true, msg.byte[2]);
                }
              }
            } else {
              if(*DREF(recording)) {
                map_insert(record->events, PAIR(i, msg.data));
              } else {
                int n = min(sizeof(msg.byte), fixed_length(msg.byte[0]));
                write_synth((seg_t) { .n = n, .s = msg.byte });
              }
            }
            change = true;
          }
        }
        p = map_next(&it, p);
      }

      // notes
      if(record->copy.first_note >= 0) {
        int transpose = DREF(set_page)->note - record->copy.first_note;
        COUNTUP(c, 16) {
          if(!(disable & 1 << c)) {
            vec128b src_notes = record->notes[src][c];
            if(transpose >= 0) {
              vec128b_shiftl(&src_notes, transpose);
            } else {
              vec128b_shiftr(&src_notes, -transpose);
            }
            //if(~record->notes[i][c] & src_notes) change = true;
            if(!vec128b_zero(&src_notes)) change = true; // *** ^
            if(*DREF(recording)) {
              vec128b_or(&record->notes[i][c], &src_notes);
            }
            vec128b_or(&record->extra[c], &src_notes);
          }
        }
      }
    }
    return change;
  }

  if(*DREF(recording)) {
    bool change = false;
    // events
    if((state->events & CURRENT_NOTE) &&
       DREF(current_note)->on &&
       !vec128b_bit_is_set(&record->notes[beat][channel], DREF(current_note)->id)) {
      msg_data_t msg = {{
          0x90 | channel,
          DREF(current_note)->id,
          DREF(current_note)->velocity
        }};
      map_insert(record->events, PAIR(beat, msg.data));
      change = true;
    }
    if(state->events & CHANNEL_PRESSURE) {
      msg_data_t msg = {{
          0xd0 | channel,
          *DREF(channel_pressure)
        }};
      map_insert(record->events, PAIR(beat, msg.data));
      change = true;
    }
    if(state->events & PITCH_BEND) {
      msg_data_t msg = {{
          0xe0 | channel,
          *DREF(pitch_bend) & 0x7f,
          (*DREF(pitch_bend) >> 7) & 0x7f
        }};
      map_insert(record->events, PAIR(beat, msg.data));
      change = true;
    }

    // notes
    int i = start;
    while(i != stop) {
      vec128b_or(&record->notes[i][channel], DREF(notes));
      i = (i + 1) % BEATS;
    }

    // mark the channel active
    if(change) record->active |= 1 << channel;

    return change;
  }

  return false;
}

DTASK(passthrough, bool) {
  if(*DREF_PASS(deleting) ||
     DREF_PASS(set_page)->note >= 0) {
    return false;
  }

  int channel = *DREF_PASS(channel);
  bool change = false;
  if(state->events & CURRENT_NOTE) {
    synth_note(channel,
               DREF(current_note)->id,
               DREF(current_note)->on,
               DREF(current_note)->velocity);
    change = true;
  }
  if(state->events & CHANNEL_PRESSURE) {
    write_synth((seg_t) { .n = 2, .s = (char [2]) {
          0xd0 | channel,
          *DREF(channel_pressure)
        }});
    change = true;
  }
  if(state->events & PITCH_BEND) {
    write_synth((seg_t) { .n = 3, .s = (char [3]) {
          0xe0 | channel,
          *DREF(pitch_bend) & 0x7f,
          (*DREF(pitch_bend) >> 7) & 0x7f
        }});
    change = true;
  }
  return change;
}

DTASK_ENABLE(set_page) {
  DREF(set_page)->note = -1;
  DREF(set_page)->keep = 0xff;
}

DTASK(set_page, struct { int val, set, keep, note; }) {
  const control_change_t *cc = DREF(control_change);
  set_page_t *p = DREF(set_page);
  if(*DREF(new_button)) {
    p->val = 0;
    p->keep = 0;
    p->set = 0;
    p->note = -1;
    return true;
  }
  if(state->events & CONTROL_CHANGE) {
    int c = cc->control;
    bool top = INRANGE(c, 20, 27);
    bool bottom = INRANGE(c, 102, 109);
    if(cc->value) {
      p->note = -1;
      if(bottom) {
        p->val = (c - 102) * 8 | (p->val & 0x07);
        p->set |= 0x38;
      } else if(top) {
        p->val = (p->val & 0x38) | (c - 20);
        p->set |= 0x07;
      }
    } else if(p->set && (top || bottom)) {
      p->val &= p->set;
      p->keep = 0x38 & ~p->set; // subpage = 0 if not set
      p->set = 0;
      return true; // event on release
    }
  }
  if(state->events & CURRENT_NOTE) {
    if(DREF(current_note)->on) {
      if(p->set) {
        p->val &= p->set;
        p->keep = 0x38 & ~p->set; // subpage = 0 if not set
        p->set = 0;
        p->note = DREF(current_note)->id;
        return true;
      } else if(p->note >= 0) {
        p->note = DREF(current_note)->id;
        return true;
      }
    }
  }
  return false;
}

DTASK_ENABLE(playback) {
  COUNTUP(i, 64) {
    set_pad_color(i, background_color(pad_to_note(i) + *DREF(transpose)));
  }
}

DTASK(playback, struct { vec128b played[16]; }) {
  vec128b *played = DREF(playback)->played;
  unsigned int beat = DREF(beat)->now;
  vec128b *notes = DREF(record)->notes[beat];
  vec128b *extra = DREF(record)->extra;
  int channel = *DREF_PASS(channel);
  unsigned int disable = *DREF_PASS(disable_channel);
  bool changed = false;

  if(*DREF(playing)) {
    map_iterator it = map_iterator_begin(DREF(record)->events, beat);
    pair_t *p = map_find_iter(&it);
    while(p) {
      msg_data_t msg = { .data = p->second };
      int control = msg.byte[0] & 0xf0;
      if(control == 0x90) {
        int c = msg.byte[0] & 0x0f;
        if(!(disable & 1ull << c)) {
          synth_note(c, msg.byte[1], true, msg.byte[2]);
          vec128b_set_bit(&played[c], msg.byte[1]);
          changed = true;
        }
      } else if(ONEOF(control, 0xd0, 0xe0)) {
        int n = min(sizeof(msg.byte), fixed_length(msg.byte[0]));
        write_synth((seg_t) { .n = n, .s = msg.byte });
      }
      p = map_next(&it, p);
    }
    COUNTUP(c, 16) {
      if(disable & 1ull << c) {
        continue;
      }
      vec128b pressed = notes[c];
      vec128b_or(&pressed, &extra[c]);
      if(c == channel) vec128b_or(&pressed, DREF(notes));
      vec128b released = played[c];
      vec128b_and_not(&released, &pressed);
      COUNTUP(i, 128) {
        if(vec128b_bit_is_set(&released, i)) {
          synth_note(c, i, false, 0);
          vec128b_clear_bit(&played[c], i);
          changed = true;
        }
      }
    }
  }
  return changed;
}

DTASK(show_playback, struct { uint8_t pad_state[64]; }) {
  uint8_t *pad_state = DREF(show_playback)->pad_state;
  unsigned int beat = DREF(beat)->now;
  vec128b *extra = DREF(record)->extra;
  int channel = *DREF(channel);
  unsigned int disable = *DREF(disable_channel);
  uint64_t pads = *DREF(pads);
  vec128b notes[16];
  vec128b all_notes = *DREF(notes);

  COUNTUP(c, 16) {
    notes[c] = DREF(record)->notes[beat][c];
    vec128b_or(&notes[c], &extra[c]);
    if(!(disable & 1u << c)) {
      vec128b_or(&all_notes, &notes[c]);
    }
  }
  if(!(disable & 1u << channel)) {
    vec128b_or(&notes[channel], DREF(notes));
  }
  bool first = true;
  bool changed = false;

  COUNTUP(i, 64) {
    uint64_t bit = 1ull << i;
    int note = pad_to_note(i) + *DREF(transpose);
    int color = 0;
    if(INRANGE(note, 0, 127)) {
      color = background_color(note);
      if(vec128b_bit_is_set(&all_notes, note)) {
        color = PAD_YELLOW;
        if(vec128b_bit_is_set(&notes[channel], note)) {
          color = PAD_RED;
        }
      }
      if(pads & bit) {
        color = PAD_GREEN;
        if(first) { // show first pressed note
          printf_text(0, 0, "note: %.2s, octave: %1d, number: %3d",
                      get_note_name(note),
                      get_note_octave(note),
                      note);
          first = false;
        }
      }
    }
    if(pad_state[i] != color) {
      set_pad_color(i, color);
      pad_state[i] = color;
      changed = true;
    }
  }
  return changed;
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
  const control_change_t *cc = DREF(control_change);
  if(cc->control == 87) {
    *DREF(new_button) = cc->value;
    if(cc->value) COUNTUP(c, 16) all_notes_off(c);
    return true;
  }
  return false;
}

DTASK_ENABLE(playing) {
  send_msg(0xb0, 85, 2);
  *DREF(playing) = false;
}

DTASK(playing, bool) {
  const control_change_t *cc = DREF(control_change);
  if(cc->control == 85 && cc->value) {
    *DREF(playing) = !*DREF(playing);
    send_msg(0xb0, 85, *DREF(playing) ? 1 : 2);
    if(*DREF(playing)) { // send programs on play
      COUNTDOWN(c, 16) {
        write_synth((seg_t) { // bank LSB
            .n = 3,
            .s = (char [3]) { 0xb0 | c, 32,
              DREF_PASS(program)->bank[c] }
          });
        write_synth((seg_t) { // program
            .n = 2,
            .s = (char [2]) { 0xc0 | c,
              DREF_PASS(program)->program[c] }
          });
        write_synth((seg_t) { // volume
            .n = 3,
            .s = (char [3]) { 0xb0 | c, 7,
              DREF_PASS(volume)->arr[c] }
          });
      }
    } else {
      COUNTDOWN(c, 16) {
        all_notes_off(c);
        write_synth((seg_t) { // volume
            .n = 3,
            .s = (char [3]) { 0xb0 | c, 7,
              DREF_PASS(volume)->arr[c] }
          });
      }
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
  const control_change_t *cc = DREF(control_change);
  if(cc->control == 86 && cc->value) {
    *DREF(recording) = !*DREF(recording);
    send_msg(0xb0, 86, *DREF(recording) ? 4 : 0);
    return true;
  }
  return false;
}

DTASK(shuttle, int8_t) {
  const control_change_t *cc = DREF(control_change);
  if(cc->control == 15 && cc->value) {
    int8_t val = cc->value;
    if(val >= 64) val = val - 128;
    *DREF(shuttle) = val;
    return true;
  }
  return false;
}

DTASK(volume, struct { int arr[16]; }) {
  const control_change_t *cc = DREF(control_change);
  int channel = *DREF_PASS(channel);
  if(cc->control == 79) {
    int val = cc->value;
    if(val >= 64) val = val - 128;
    int *p = &DREF(volume)->arr[channel];
    *p = clamp(0, 127, *p + val);
    write_synth((seg_t) {
      .n = 3,
      .s = (char [3]) { 0xb0 | channel, 7, *p }
    });
    return true;
  }
  return false;
}

DTASK_ENABLE(show_volume) {
  printf_text(51, 3, "volume: %3d", DREF(volume)->arr[*DREF(channel)]);
}

DTASK(show_volume, bool) {
  printf_text(51, 3, "volume: %3d", DREF(volume)->arr[*DREF(channel)]);
  return true;
}

DTASK(program, struct { int bank[16], program[16]; }) {
  const control_change_t *cc = DREF(control_change);
  int channel = *DREF_PASS(channel);
  if(cc->control == 78) {
    int val = cc->value;
    if(val >= 64) val -= 128;
    int *b = &DREF(program)->bank[channel];
    int *p = &DREF(program)->program[channel];
    int pb = (*b << 7) | *p;
    pb = (pb + val + BANKS * 128) % (BANKS * 128);
    *b = pb >> 7;
    *p = pb & 0x7f;
    write_synth((seg_t) { // bank LSB
        .n = 3,
        .s = (char [3]) { 0xb0 | channel, 32, *b }
      });
    write_synth((seg_t) { // program
        .n = 2,
        .s = (char [2]) { 0xc0 | channel, *p }
      });
    return true;
  }
  return false;
}

DTASK_ENABLE(show_program) {
  __dtask_show_program(state);
}

DTASK(show_program, bool) {
  printf_text(51, 0, "program: %c%3d",
              'A' + DREF(program)->bank[*DREF(channel)],
              DREF(program)->program[*DREF(channel)]);
  return true;
}

DTASK_ENABLE(channel) {
  send_msg(0xb0, 46, 1);
  send_msg(0xb0, 47, 1);
  *DREF(channel) = 0;
  printf_text(51, 1, "channel: %2d", *DREF(channel) + 1);
}

DTASK(channel, int) {
  const control_change_t *cc = DREF(control_change);
  if(ONEOF(cc->control, 46, 47) && cc->value) {
    *DREF(channel) = (*DREF(channel) + (cc->control == 47 ? 15 : 1)) % 16;
    printf_text(51, 1, "channel: %2d", *DREF(channel) + 1);
    return true;
  }
  return false;
}

DTASK_ENABLE(show_disable_channel) {
  send_msg(0xb0, 48, 4);
  *DREF(disable_channel) = 0;
}

DTASK(show_disable_channel, bool) {
  unsigned int bit = 1ull << *DREF(channel);
  send_msg(0xb0, 48, *DREF(disable_channel) & bit ? (DREF(record)->active & bit ? 2 : 0) : 4);
  return true;
}

DTASK(disable_channel, unsigned int) {
  int channel = *DREF_PASS(channel);
  const control_change_t *cc = DREF(control_change);
  if(cc->control == 48 && cc->value) {
    *DREF(disable_channel) ^= 1ull << channel;
    return true;
  }
  return false;
}

DTASK_ENABLE(transpose) {
  send_msg(0xb0, 44, 1);
  send_msg(0xb0, 45, 1);
  *DREF(transpose) = 43;
  printf_text(51, 2, "octave: %1d", get_note_octave(*DREF(transpose) + 5));
}

DTASK(transpose, int8_t) {
  const control_change_t *cc = DREF(control_change);
  if(ONEOF(cc->control, 44, 45) && cc->value) {
    int8_t *off = DREF(transpose);
    *off = clamp(-5, 79, (int)*off + (cc->control == 44 ? -12 : 12));
    printf_text(51, 2, "octave: %1d", get_note_octave(*off + 5)); // get octave of lowest C
    return true;
  }
  return false;
}

DTASK_ENABLE(poweroff) {
  *DREF(poweroff) = false;
}

DTASK(poweroff, bool) {
  const control_change_t *cc = DREF(control_change);
  if(cc->control == 3 && cc->value) {
    *DREF(poweroff) = true;
    return true;
  }
  return false;
}

// Local Variables:
// eval: (add-to-list 'imenu-generic-expression '("Task" "^DTASK(\\([a-zA-Z0-9_]+\\),.*$" 1))
// End:
