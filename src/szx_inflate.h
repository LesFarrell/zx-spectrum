#ifndef SZX_INFLATE_H
#define SZX_INFLATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Expands one RFC 1950 zlib stream into an exactly sized caller buffer. */
bool szx_inflate_zlib(
    const uint8_t *source,
    size_t source_size,
    uint8_t *destination,
    size_t destination_size
);

#endif
