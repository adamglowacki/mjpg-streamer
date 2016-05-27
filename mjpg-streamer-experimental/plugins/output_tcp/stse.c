#include "stse.h"

static const uint8_t stx = 0x01;
static const uint8_t etx = 0x02;
static const uint8_t escape = 0x03;
static const uint8_t escaped_stx = 0x11;
static const uint8_t escaped_etx = 0x12;
static const uint8_t escaped_escape = 0x13;

static inline bool append8(stse_buf_t *buf, uint8_t x) {
  if (buf->used >= buf->size)
    return false;
  buf->bytes[buf->used++] = x;
  return true;
}

bool stse_start(stse_buf_t *buf) {
  return append8(buf, stx);
}

static inline bool append8_twice(stse_buf_t *buf, uint8_t x, uint8_t y) {
  if (!append8(buf, x))
    return false;
  if (!append8(buf, y))
    return false;
  return true;
}

static inline bool append_payload(stse_buf_t *buf, uint8_t x) {
  if (x == stx)
    return append8_twice(buf, escape, escaped_stx);
  else if (x == etx)
    return append8_twice(buf, escape, escaped_etx);
  else if (x == escape)
    return append8_twice(buf, escape, escaped_escape);
  else
    return append8(buf, x);
}

bool stse_append(stse_buf_t *buf, uint8_t *bytes, uint32_t bytes_count) {
  uint32_t i;
  for (i = 0; i < bytes_count; i++)
    if (!append_payload(buf, bytes[i]))
      return false;
  return true;
}

bool stse_end(stse_buf_t *buf) {
  return append8(buf, etx);
}

static inline void init(stse_decode_t *dec) {
  dec->meta.is_started = false;
  dec->buf.used = 0;
}

void stse_decode_init(stse_decode_t *dec) {
  init(dec);
}

static inline bool reset_and_false(stse_decode_t *dec) {
  init(dec);
  return false;
}

static inline void append8_or_reset(stse_decode_t *dec, uint8_t x) {
  if (!append8(&dec->buf, x))
    init(dec);
}

bool stse_decode(stse_decode_t *dec, uint8_t x) {
  if (x == stx) {
    init(dec);
    dec->meta.is_started = true;
    return false;
  } else if (!dec->meta.is_started) {
    return false;
  }

  if (x == etx) {
    dec->meta.is_started = false;
    return !dec->meta.is_escaping;
  }
  
  /* inside frame */
  if (dec->meta.is_escaping) {
    if (x == escaped_stx)
      append8_or_reset(dec, stx);
    else if (x == escaped_etx)
      append8_or_reset(dec, etx);
    else if (x == escaped_escape)
      append8_or_reset(dec, escape);
    else
      init(dec);
    dec->meta.is_escaping = false;
  } else if (x == escape) {
    dec->meta.is_escaping = true;
  } else {
    append8_or_reset(dec, x);
  }
  return false;
}
