#ifndef STSE_H
#define STSE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
  uint8_t *bytes;
  uint32_t size;
  uint32_t used;
} stse_buf_t;

typedef struct {
  struct {
    bool is_started;
    bool is_escaping;
  } meta;
  stse_buf_t buf;
} stse_decode_t;

/* append the start of segment to [buf] */
bool stse_start(stse_buf_t *buf);
bool stse_append(stse_buf_t *buf, uint8_t *bytes, uint32_t bytes_count);
bool stse_end(stse_buf_t *buf);

void stse_decode_init(stse_decode_t *dec);
/* return whether an entire frame has just been received */
bool stse_decode(stse_decode_t *dec, uint8_t x);

#endif
