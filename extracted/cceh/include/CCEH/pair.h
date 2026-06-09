#ifndef UTIL_PAIR_H_
#define UTIL_PAIR_H_

#include <immintrin.h>

typedef size_t Key_t;
typedef const char* Value_t;

const Key_t SENTINEL = -2;  // 11111...110
const Key_t INVALID = -1;   // 11111...111

const Value_t NONE = 0x0;
const Value_t DEFAULT = reinterpret_cast<Value_t>(1);

/*variable length key*/
struct string_key {
  int length;
  char key[0];
};

#endif  // UTIL_PAIR_H_
