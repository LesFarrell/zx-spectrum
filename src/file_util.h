#ifndef FILE_UTIL_H
#define FILE_UTIL_H

#include <stdbool.h>
#include <stddef.h>

typedef struct FileWriteChunk {
    const void *data;
    size_t size;
} FileWriteChunk;

/* Writes all chunks to a temporary file beside the destination, then replaces
   the destination only after the complete temporary file has been flushed. */
bool file_write_atomic(
    const char *path,
    const FileWriteChunk *chunks,
    size_t chunk_count,
    const char *description,
    char *error_buffer,
    size_t error_buffer_size
);

#endif
