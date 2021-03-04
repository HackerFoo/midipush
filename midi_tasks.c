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

#ifndef DTASK_GEN
#include "types.h"
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include "startle/types.h"
#include "startle/macros.h"
#include "startle/map.h"
#include "startle/support.h"
#include "midipush.h"
#include "vec128b.h"
#include "midi_tasks.h"
#endif

#define PAD_RED 5
#define PAD_YELLOW 13
#define PAD_GREEN 21
#define PAD_PURPLE 53

#define DEBOUNCE_MS 100

#define MOD_INC(x, m, n) ((x + n) % m)
#define MOD_DEC(x, m, n) ((x + m - n) % m)
#define MOD_OFFSET(x, m, n) ((x + m + n) % m)

enum infer_scale_mode {
  INFER_SCALE_OFF = 0,
  INFER_SCALE_ON,
  INFER_SCALE_LOCK,
  INFER_SCALE_MAX
};

static_assert(MIDI_TASKS_COUNT <= 64, "too many tasks");

DTASK_GROUP(midi_tasks)

// passthrough
DTASK(midi_in, struct { int id; unsigned char status; seg_t data; }) {
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

DTASK(external_tick, bool) {
  return true;
}

unsigned int page_beat(unsigned int b, const set_page_t *p) {
  unsigned int current_page = b / BEATS_PER_PAGE;
  return ((current_page & p->keep) | p->val) * BEATS_PER_PAGE;
}

DTASK(beat, struct { unsigned int then, now; }) {
  DREF(beat)->then = DREF(beat)->now;
  if((state->events & (TICK | EXTERNAL_TICK)) && *DREF(playing)) {
    (void)DREF(tick);
    (void)DREF(external_tick);
    DREF(beat)->now = MOD_INC(DREF(beat)->then, BEATS, 1);
    if(!(DREF(beat)->now % BEATS_PER_PAGE)) { // next page
      int last_page = DREF(beat)->then / BEATS_PER_PAGE;
      int next_page = inc_mask(last_page, *DREF(page_mask)) % PAGES;
      if(MOD_INC(last_page, PAGES, 1) != next_page) {
        DREF(beat)->then = DREF(beat)->now = next_page * BEATS_PER_PAGE;
      }
    }
  }
  if(state->events & SHUTTLE) {
    DREF(beat)->now = MOD_OFFSET(DREF(beat)->then, BEATS, *DREF(shuttle) * BEATS_PER_PAGE / 4);
  }
  if((state->events & SET_PAGE) && DREF(set_page)->note == -1 && DREF(set_page)->keep != 0xff) {
    DREF(beat)->now = page_beat(DREF(beat)->then, DREF(set_page));
  }
  return true;
}

DTASK(print_midi_msg, bool) {
  const midi_in_t *msg = DREF(midi_in);
  if(msg->status != 0xf8) {
    printf("%d > 0x%x:", msg->id, msg->status);
    COUNTUP(i, msg->data.n) {
      printf(" %d", (unsigned char)msg->data.s[i]);
    }
    printf("\n");
  }
  return true;
}

void set_bit(uint64_t *s, int k, bool v) {
  if(v) {
    *s |= 1ull << k;
  } else {
    *s &= ~(1ull << k);
  }
}

DTASK(pad, key_event_t) {
  const midi_in_t *msg = DREF(midi_in);
  if(msg->id != 0) return false;
  unsigned char control = msg->status & 0xf0;
  if(ONEOF(control, 0x80, 0x90)) {
    int p = msg->data.s[0] - 36;
    if(INRANGE(p, 0, 63)) {
      *DREF(pad) = (key_event_t) {
        .id = p,
        .velocity = (control == 0x90 ? 1 : -1) * (int16_t)msg->data.s[1]
      };
      return true;
    }
  }
  return false;
}

DTASK(external_key, key_event_t) {
  const midi_in_t *msg = DREF(midi_in);
  if(msg->id == 0) return false;
  unsigned char control = msg->status & 0xf0;
  if(ONEOF(control, 0x80, 0x90)) {
    *DREF(external_key) = (external_key_t) {
      .id = msg->data.s[0],
      .velocity = (control == 0x90 ? 1 : -1) * (int16_t)msg->data.s[1]
    };
    return true;
  }
  return false;
}

DTASK(channel_pressure, int) {
  const midi_in_t *msg = DREF(midi_in);
  if(msg->id != 0) return false;
  unsigned char control = msg->status & 0xf0;
  if(control == 0xd0) {
    *DREF(channel_pressure) = msg->data.s[0];
    return true;
  }
  return false;
}

DTASK(pitch_bend, int) {
  const midi_in_t *msg = DREF(midi_in);
  if(msg->id != 0) return false;
  unsigned char control = msg->status & 0xf0;
  if(control == 0xe0) {
    *DREF(pitch_bend) = (msg->data.s[0] & 0x7f) | (((int)msg->data.s[1] & 0x7f) << 7) ;
    return true;
  }
  return false;
}

DTASK(control_change, struct { int control, value; } ) {
  const midi_in_t *msg = DREF(midi_in);
  if(msg->id != 0) return false;
  unsigned char control = msg->status & 0xf0;
  if(control == 0xb0) {
    DREF(control_change)->control = msg->data.s[0];
    DREF(control_change)->value = msg->data.s[1];
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
    set_bit(DREF(pads), pad->id, pad->velocity > 0);
  }
  return *DREF(pads) != prev;
}

DTASK_ENABLE(current_note) {
  key_event_t empty = { -1, 0 };
  DELAY_FILL(DREF(current_note), key_event_t, HISTORY, &empty);
}

DTASK(current_note, DELAY(key_event_t, HISTORY)) {
  if(state->events & PAD) {
    const key_event_t *pad = DREF(pad);
    int id = pad_to_note(pad->id) + *DREF(transpose);
    if(INRANGE(id, 0, 127)) {
      key_event_t e = {
        .id = pad_to_note(pad->id) + *DREF(transpose),
        .velocity = pad->velocity,
        .tick = *DREF_PASS(tick)
      };

      // pseudo debounce - pads can bounce with lesser velocity cancelling the previous note,
      // so this takes the maximum of the two (assuming only two note ons)
      const key_event_t *prev = DELAY_READ(DREF(current_note), key_event_t, HISTORY, 1);
      if(e.velocity > 0 &&
         prev->id == e.id &&
         e.tick < prev->tick + DEBOUNCE_MS) {
        e.velocity = max(e.velocity, prev->velocity);
      }

      DELAY_WRITE(DREF(current_note), key_event_t, HISTORY, &e);
      return true;
    }
  }
  if(state->events & EXTERNAL_KEY) {
    const key_event_t *key = DREF(external_key);
    key_event_t e = {
      .id = key->id,
      .velocity = key->velocity,
      .tick = *DREF_PASS(tick)
    };
    DELAY_WRITE(DREF(current_note), key_event_t, HISTORY, &e);
    return true;
  }
  return false;
}

DTASK_ENABLE(notes) {
  vec128b_set_zero(&DREF(notes)->v);
  DREF(notes)->cnt = 0;
}

DTASK(notes, struct { vec128b v; int cnt; }) {
  (void)DREF(transpose);
  bool changed = false;
  const key_event_t *note = DELAY_READ(DREF(current_note), key_event_t, HISTORY, 0);
  vec128b prev = DREF(notes)->v;
  if(state->events & TRANSPOSE) {
    vec128b_set_zero(&DREF(notes)->v);
  } else {
    vec128b_set_bit_val(&DREF(notes)->v, note->id, note->velocity > 0);
  }
  if(!vec128b_eq(&DREF(notes)->v, &prev)) {
    DREF(notes)->cnt += note->velocity > 0 ? 1 : -1;
    changed = true;
  }
  if(vec128b_zero(&DREF(notes)->v)) {
    DREF(notes)->cnt = 0;
  }
  return changed;
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

static
void delete_notes(record_t *record, unsigned int beat, unsigned int channel, const vec128b *notes) {
  vec128b_and_not(&record->notes[beat][channel], notes);
  map_iterator it = map_iterator_begin(record->events, beat);
  pair_t *p = map_find_iter(&it);
  while(p) {
    msg_data_t msg = { .data = p->second };
    if(msg.byte[0] == (0x90 | channel) &&
       vec128b_bit_is_set(notes, msg.byte[1])) {
      p->second = 0;
    }
    p = map_next(&it, p);
  }
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
          start = MOD_INC(start, BEATS, 1);
        }
      found_start:
        record->copy.first_beat = start;
      }
      record->copy.shift = MOD_DEC(beat, BEATS, record->copy.first_beat - 1);
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
     DREF(notes)->cnt != 0) {

    // clear the tails from notes that will be deleted
    vec128b clear = DREF(notes)->v;
    vec128b_and(&clear, &record->notes[MOD_DEC(stop, BEATS, 1)][channel]);
    for(int i = stop;
        !vec128b_zero(&clear);
        i = MOD_INC(i, BEATS, 1)) {
      vec128b_and(&clear, &record->notes[i][channel]);
      vec128b_and_not(&record->notes[i][channel], &clear);
    }

    // clear the heads from notes that will be deleted
    clear = DREF(notes)->v;
    vec128b_and(&clear, &record->notes[start][channel]);
    for(int i = MOD_DEC(start, BEATS, 1);
        !vec128b_zero(&clear);
        i = MOD_DEC(i, BEATS, 1)) {
      vec128b_and(&clear, &record->notes[i][channel]);
      delete_notes(record, i, channel, &clear);
    }

    // delete the notes inside the period
    for(int i = start; i != stop; i = MOD_INC(i, BEATS, 1)) {
      delete_notes(record, i, channel, &DREF(notes)->v);
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
    start = MOD_INC(start, BEATS, 1);
    stop = MOD_INC(stop, BEATS, 1);
    for(int i = start; i != stop; i = MOD_INC(i, BEATS, 1)) {
      int src = MOD_DEC(i, BEATS, record->copy.shift);

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
    if(state->events & CURRENT_NOTE) {
      const key_event_t *note = DELAY_READ(DREF(current_note), key_event_t, HISTORY, 0);
      if(note->velocity > 0 &&
         !vec128b_bit_is_set(&record->notes[beat][channel], note->id) &&
         (*DREF_PASS(infer_scale_mode) != INFER_SCALE_LOCK ||
          in_key(*DREF_PASS(infer_scale), note->id))) {
        msg_data_t msg = {{
            0x90 | channel,
            note->id,
            note->velocity
          }};
        map_insert(record->events, PAIR(beat, msg.data));
        change = true;
      }
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
      vec128b_or(&record->notes[i][channel], &DREF(notes)->v);
      i = MOD_INC(i, BEATS, 1);
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
  if(DTASK_AND(NOTES | CURRENT_NOTE)) {
    const key_event_t *note = DELAY_READ(DREF(current_note), key_event_t, HISTORY, 0);
    if(*DREF_PASS(infer_scale_mode) != INFER_SCALE_LOCK ||
       in_key(*DREF_PASS(infer_scale), note->id)) {
      synth_note(channel,
                 note->id,
                 note->velocity > 0,
                 abs(note->velocity));
      change = true;
    }
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
    const key_event_t *note = DELAY_READ(DREF(current_note), key_event_t, HISTORY, 0);
    if(note->velocity > 0) {
      if(p->set) {
        p->val &= p->set;
        p->keep = 0x38 & ~p->set; // subpage = 0 if not set
        p->set = 0;
        p->note = note->id;
        return true;
      } else if(p->note >= 0) {
        p->note = note->id;
        return true;
      }
    }
  }
  return false;
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
    // setup channels
    if(state->events & PLAYING) {
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
    }
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
      if(c == channel) vec128b_or(&pressed, &DREF(notes)->v);
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
  } else if(state->events & PLAYING) {
    // clear notes
    COUNTDOWN(c, 16) {
      all_notes_off(c);
    }
  }
  return changed;
}

// TODO optimize
int min_note(vec128b *v) {
  COUNTUP(n, 128) {
    if(vec128b_bit_is_set(v, n)) {
      return n;
    }
  }
  return -1;
}

DTASK_ENABLE(show_playback) {
  COUNTUP(i, 64) {
    int c = background_color(pad_to_note(i) + *DREF(transpose), *DREF(infer_scale));
    DREF(show_playback)->pad_state[i] = c;
    set_pad_color(i, c);
  }
}

DTASK(show_playback, struct { uint8_t pad_state[64]; }) {
  uint8_t *pad_state = DREF(show_playback)->pad_state;
  unsigned int beat = DREF(beat)->now;
  vec128b *extra = DREF(record)->extra;
  int channel = *DREF(channel);
  unsigned int disable = *DREF(disable_channel);
  uint64_t pads = *DREF(pads);
  vec128b notes[16];
  vec128b all_notes = DREF(notes)->v;

  COUNTUP(c, 16) {
    notes[c] = DREF(record)->notes[beat][c];
    vec128b_or(&notes[c], &extra[c]);
    if(!(disable & 1u << c)) {
      vec128b_or(&all_notes, &notes[c]);
    }
  }
  if(!(disable & 1u << channel)) {
    vec128b_or(&notes[channel], &DREF(notes)->v);
  }

  int scale = *DREF(infer_scale);
  bool changed = false;
  COUNTUP(i, 64) {
    uint64_t bit = 1ull << i;
    int note = pad_to_note(i) + *DREF(transpose);
    int color = 0;
    if(INRANGE(note, 0, 127)) {
      color = background_color(note, scale);
      if(vec128b_bit_is_set(&all_notes, note)) {
        color = PAD_YELLOW;
        if(vec128b_bit_is_set(&notes[channel], note)) {
          color = PAD_RED;
        }
      }
      if(pads & bit) {
        color = in_key(*DREF(infer_scale), note) ? PAD_GREEN : PAD_PURPLE;
      }
    }
    if(pad_state[i] != color) {
      set_pad_color(i, color);
      pad_state[i] = color;
      changed = true;
    }
  }
  int base_note = min_note(&DREF(notes)->v);
  if(base_note >= 0) { // show base note
    printf_text(0, 0, "note: %.2s, octave: %2d, number: %3d",
                get_note_name(base_note),
                get_note_octave(base_note),
                base_note);
  }
  printf_text(0, 1, "scale: %.2s", get_note_name(scale));
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
  if(!(state->events & CONTROL_CHANGE)) return true; // allow external triggering
  if(cc->control == 85 && cc->value) {
    *DREF(playing) = !*DREF(playing);
    send_msg(0xb0, 85, *DREF(playing) ? 1 : 2);
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
  send_msg(0xb0, 44, 1);
  send_msg(0xb0, 45, 1);
  *DREF(channel) = 0;
  printf_text(51, 1, "channel: %2d", *DREF(channel) + 1);
}

DTASK(channel, int) {
  const control_change_t *cc = DREF(control_change);
  if(ONEOF(cc->control, 44, 45) && cc->value) {
    *DREF(channel) = (*DREF(channel) + (cc->control == 44 ? 15 : 1)) % 16;
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

#define ALIGNMENT_PADDING(x, m) ((m - x % m) % m)

static void show_octave(int off) {
  printf_text(51, 2, "octave: %1d%s", get_note_octave(off), off % 12 ? ".5" : "  ");
}

DTASK_ENABLE(transpose) {
  send_msg(0xb0, 46, 1);
  send_msg(0xb0, 47, 1);
  *DREF(transpose) = 43;
  show_octave(*DREF(transpose));
}

const int upper_limit = 128 - 40 + ALIGNMENT_PADDING(128 - 40, 12);

DTASK(transpose, int8_t) {
  const control_change_t *cc = DREF(control_change);
  if(ONEOF(cc->control, 46, 47) && cc->value) {
    int8_t *off = DREF(transpose);
    int step = *off % 12 ? 5 : 7;
    *off = clamp(0, upper_limit, (int)*off + (cc->control == 47 ? step - 12 : step));
    show_octave(*off);
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

DTASK_ENABLE(save) {
  send_msg(0xb0, 53, 1);
}

DTASK(save, bool) {
  const control_change_t *cc = DREF(control_change);
  if(cc->control == 53 && cc->value) {
    *DREF(save) = true;
    return true;
  }
  return false;
}

DTASK_ENABLE(page_mask) {
  COUNTUP(i, 6) {
    send_msg(0xb0, 36 + i, !!(*DREF(page_mask) & (1 << i)));
  }
}

DTASK(page_mask, unsigned int) {
  const control_change_t *cc = DREF(control_change);
  if(INRANGE(cc->control, 36, 41) && cc->value) {
    int bit = 1 << (cc->control - 36);
    *DREF(page_mask) ^= bit;
    send_msg(0xb0, cc->control, !!(*DREF(page_mask) & bit));
    return true;
  }
  return false;
}

DTASK_ENABLE(set_metronome) {
  DREF(set_metronome)->channel = -1;
}

DTASK(set_metronome, struct { int channel, note; }) {
  const control_change_t *cc = DREF(control_change);
  set_metronome_t *m = DREF(set_metronome);
  if(cc->control == 9 && cc->value) {
    if(DREF_PASS(notes)->cnt == 0) {
      m->channel = -1;
    } else {
      int n = min_note(&DREF_PASS(notes)->v);
      if(n >= 0) {
          m->channel = *DREF_PASS(channel);
          m->note = n;
      }
    }
    return true;
  }
  return false;
}

DTASK(metronome, bool) {
  set_metronome_t *m = DREF(set_metronome);
  if(m->channel >= 0 && *DREF(playing)) {
    int t = DREF(beat)->now % BEATS_PER_PAGE;
    if(t == 0) {
      *DREF(metronome) = true;
      synth_note(m->channel, m->note, true, 64);
      return true;
    } else if(*DREF(metronome) && t >= BEATS_PER_PAGE / 16) {
      *DREF(metronome) = false;
      synth_note(m->channel, m->note, false, 0);
      return true;
    }
  }
  return false;
}

DTASK_ENABLE(infer_scale) {
  *DREF(infer_scale) = 0;
}

// Switch as fast as possible, especially on chords
// Handle seventh chords and playing in scale (reasonably)
// Pressing 0/4/7 repeatedly doesn't change scale
// Minor scale selects the corresponding majpr scale
DTASK(infer_scale, int) {
  // simple fixed point representation
#define F(x) (4*(x))
  static const int8_t f[12] = { // convolution filter in fixed point
     F(2.5),   F(-1),  F(1), F(-1),
    F(2.25),    F(1), F(-1),  F(2),
      F(-1), F(1.25), F(-1),  F(1)
  };
  static const int8_t f_chord = F(6.75); // f[0] + f[4] + f[7], a major chord
  if((state->events & CURRENT_NOTE) &&
     *DREF(infer_scale_mode) &&
      DELAY_READ(DREF(current_note), key_event_t, HISTORY, 0)->velocity > 0) {
    const int prev_scale = *DREF(infer_scale);
    int c[12] = {0};
    int cnt = 0;
    int set = 0;
    for(int i = 0; i < HISTORY && cnt < 7; i++) {
      const key_event_t *e = DELAY_READ(DREF(current_note), key_event_t, HISTORY, i);
      if(e->id <= 0) break;
      if(e->velocity <= 0) continue;
      int s = e->id % 12;
      int bit = 1 << s;
      if(!(set & bit)) {
        cnt++;
        set |= bit;
        bool done = false;
        COUNTUP(j, 12) {
          c[j] += f[(12 + s - j) % 12];
          if(cnt == 3 && c[j] >= f_chord) {
            done = true; // matched a chord, finish
          }
        }
        if(done) break;
      }
    }

    // bias towards previous scale
    c[prev_scale] += cnt * F(0.5); // cnt * (f[0] - f[4]), so that repeated 4 or 7 won't switch
    int c_max = c[prev_scale], c_i = prev_scale;
    COUNTUP(i, 12) {
      if(c[i] > c_max) {
        c_max = c[i];
        c_i = i;
      }
    }
    if(prev_scale != c_i) {
      *DREF(infer_scale) = c_i;
      return true;
    }
  }
  return false;
#undef F
}

static
void infer_scale_indicate(enum infer_scale_mode state) {
  static const int t[INFER_SCALE_MAX] = {2, 4, 5};
  if(state < INFER_SCALE_MAX) {
    send_msg(0xb0, 58, t[state]);
  }
}


DTASK_ENABLE(infer_scale_mode) {
  infer_scale_indicate(*DREF(infer_scale_mode) = INFER_SCALE_ON);
}

DTASK(infer_scale_mode, int) {
  const control_change_t *cc = DREF(control_change);
  if(cc->control == 58 && cc->value) {
    *DREF(infer_scale_mode) = (*DREF(infer_scale_mode) + 1) % INFER_SCALE_MAX;
    infer_scale_indicate(*DREF(infer_scale_mode));
    return true;
  }
  return false;
}

// Local Variables:
// eval: (add-to-list 'imenu-generic-expression '("Task" "^DTASK(\\([a-zA-Z0-9_]+\\),.*$" 1))
// End:
