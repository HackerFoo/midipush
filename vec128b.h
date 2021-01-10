#ifndef VEC128B_H
#define VEC128B_H

#include <stdbool.h>

#define WORD_BITS (sizeof(unsigned int) * 8)
#define VEC128B_WORDS (128 / WORD_BITS)

typedef struct {
  unsigned int word[VEC128B_WORDS];
} vec128b;

static inline
void vec128b_and(vec128b *a, const vec128b *b) {
#pragma GCC unroll (16 / sizeof(unsigned int))
#pragma unroll
  FOREACH(i, a->word) {
    a->word[i] &= b->word[i];
  }
}

static inline
void vec128b_and_not(vec128b *a, const vec128b *b) {
#pragma GCC unroll (16 / sizeof(unsigned int))
#pragma unroll
  FOREACH(i, a->word) {
    a->word[i] &= ~b->word[i];
  }
}

static inline
void vec128b_or(vec128b *a, const vec128b *b) {
#pragma GCC unroll (16 / sizeof(unsigned int))
#pragma unroll
  FOREACH(i, a->word) {
    a->word[i] |= b->word[i];
  }
}

static inline
void vec128b_xor(vec128b *a, const vec128b *b) {
#pragma GCC unroll (16 / sizeof(unsigned int))
#pragma unroll
  FOREACH(i, a->word) {
    a->word[i] ^= b->word[i];
  }
}

static inline
void vec128b_not(vec128b *a) {
#pragma GCC unroll (16 / sizeof(unsigned int))
#pragma unroll
  FOREACH(i, a->word) {
    a->word[i] = ~a->word[i];
  }
}

static inline
void vec128b_set_bit(vec128b *a, int b) {
  a->word[b / WORD_BITS] |= 1u << (b % WORD_BITS);
}

static inline
void vec128b_clear_bit(vec128b *a, int b) {
  a->word[b / WORD_BITS] &= ~(1u << (b % WORD_BITS));
}

static inline
void vec128b_set_bit_val(vec128b *s, int k, bool v) {
  if(v) {
    vec128b_set_bit(s, k);
  } else {
    vec128b_clear_bit(s, k);
  }
}

static inline
bool vec128b_eq(const vec128b *a, const vec128b *b) {
  bool res = true;
#pragma GCC unroll (16 / sizeof(unsigned int))
#pragma unroll
  FOREACH(i, a->word) {
    res &= a->word[i] == b->word[i];
  }
  return res;
}

static inline
bool vec128b_zero(const vec128b *a) {
  bool res = true;
#pragma GCC unroll (16 / sizeof(unsigned int))
#pragma unroll
  FOREACH(i, a->word) {
    res &= !a->word[i];
  }
  return res;
}

static inline
void vec128b_shiftl(vec128b *a, int b) {
  const int word_shiftl = b / WORD_BITS;
  const int bit_shiftl = b - word_shiftl * WORD_BITS;
  const int bit_shiftr = WORD_BITS - bit_shiftl;
  if(bit_shiftl) {
#pragma GCC unroll (16 / sizeof(unsigned int))
#pragma unroll
    COUNTDOWN(i, LENGTH(a->word)) {
      int j = i - word_shiftl;
      a->word[i] =
        (j >= 0 ? a->word[j] << bit_shiftl : 0) |
        (j-1 >= 0 ? a->word[j-1] >> bit_shiftr : 0);
    }
  } else {
#pragma GCC unroll (16 / sizeof(unsigned int))
#pragma unroll
    COUNTDOWN(i, LENGTH(a->word)) {
      int j = i - word_shiftl;
      a->word[i] = j >= 0 ? a->word[j] : 0;
    }
  }
}

static inline
void vec128b_shiftr(vec128b *a, int b) {
  const int word_shiftr = b / WORD_BITS;
  const int bit_shiftr = b - word_shiftr * WORD_BITS;
  const int bit_shiftl = WORD_BITS - bit_shiftr;
  if(bit_shiftr) {
#pragma GCC unroll (16 / sizeof(unsigned int))
#pragma unroll
    COUNTUP(i, LENGTH(a->word)) {
      int j = i + word_shiftr;
      a->word[i] =
        (j+1 < LENGTH(a->word) ? a->word[j+1] << bit_shiftl : 0) |
        (j < LENGTH(a->word) ? a->word[j] >> bit_shiftr : 0);
    }
  } else {
#pragma GCC unroll (16 / sizeof(unsigned int))
#pragma unroll
    COUNTUP(i, LENGTH(a->word)) {
      int j = i + word_shiftr;
      a->word[i] = j < LENGTH(a->word) ? a->word[j] : 0;
    }
  }
}

static inline
void vec128b_set_zero(vec128b *a) {
  memset(a, 0, sizeof(vec128b));
}

static inline
bool vec128b_bit_is_set(const vec128b *a, int b) {
  const int n = b / WORD_BITS;
  const int bit = b - n * WORD_BITS;
  return !!(a->word[n] & (1u << bit));
}

#endif
