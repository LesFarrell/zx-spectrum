#ifndef ZIP_H
#define ZIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    ZIP_ENTRY_NAME_CAPACITY = 512,
    ZIP_MAX_EXTRACTED_SIZE = 128 * 1024 * 1024
};

typedef struct ZipEntry {
    char name[ZIP_ENTRY_NAME_CAPACITY];
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t local_header_offset;
    uint16_t flags;
    uint16_t method;
    bool is_directory;
} ZipEntry;

typedef struct ZipArchive {
    uint8_t *data;
    size_t size;
    ZipEntry *entries;
    size_t entry_count;
} ZipArchive;

void zip_init(ZipArchive *archive);
void zip_discard(ZipArchive *archive);

bool zip_open_file(
    ZipArchive *archive,
    const char *path,
    char *error_buffer,
    size_t error_buffer_size
);

bool zip_extract_entry(
    const ZipArchive *archive,
    size_t entry_index,
    uint8_t **data,
    size_t *size,
    char *error_buffer,
    size_t error_buffer_size
);

#endif
