#ifndef DISK_H
#define DISK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct DskSector {
    uint8_t c;
    uint8_t h;
    uint8_t r;
    uint8_t n;
    uint8_t st1;
    uint8_t st2;
    uint32_t size;
    uint32_t stored_size;
    uint8_t *data;
} DskSector;

typedef struct DskTrack {
    uint8_t c;
    uint8_t h;
    uint8_t sector_count;
    DskSector *sectors;
} DskTrack;

typedef struct DskImage {
    uint8_t *data;
    size_t data_size;
    DskTrack *tracks;
    size_t track_slot_count;
    uint8_t cylinder_count;
    uint8_t side_count;
    bool extended;
    bool inserted;
    bool dirty;
    char path[260];
} DskImage;

void dsk_init(DskImage *image);
void dsk_discard(DskImage *image);

bool dsk_load_data(
    DskImage *image,
    const uint8_t *data,
    size_t data_size,
    char *error_buffer,
    size_t error_buffer_size
);

bool dsk_load_file(
    DskImage *image,
    const char *path,
    char *error_buffer,
    size_t error_buffer_size
);

bool dsk_drive_ready(void *user_data, uint8_t drive);

bool dsk_read_sector(
    void *user_data,
    uint8_t drive,
    uint8_t c,
    uint8_t h,
    uint8_t r,
    uint8_t n,
    uint8_t *buffer,
    uint32_t buffer_size,
    uint32_t *data_size,
    uint8_t *st1,
    uint8_t *st2
);

bool dsk_write_sector(
    void *user_data,
    uint8_t drive,
    uint8_t c,
    uint8_t h,
    uint8_t r,
    uint8_t n,
    const uint8_t *buffer,
    uint32_t data_size
);

bool dsk_get_sector_id(
    void *user_data,
    uint8_t drive,
    uint8_t c,
    uint8_t h,
    uint8_t index,
    uint8_t *out_c,
    uint8_t *out_h,
    uint8_t *out_r,
    uint8_t *out_n,
    uint8_t *st1,
    uint8_t *st2
);

#endif
