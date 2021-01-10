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

#ifndef __TYPES__
#define __TYPES__
#include <stdint.h>
#include "delay.h"

#define MAX_MIDI_DATA_LENGTH 32

typedef struct midi_msg {
  int data_length;
  uint8_t control;
  uint8_t data[MAX_MIDI_DATA_LENGTH];
} midi_msg_t;

typedef struct timeval timeval_t;

#define BEATS_PER_PAGE 24
#define PAGES 64
#define BEATS (BEATS_PER_PAGE * PAGES)
#define BANKS 26

#define INFER_SCALE_HISTORY 7
DECLARE_DELAY(int, INFER_SCALE_HISTORY);

#endif
