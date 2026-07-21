#ifndef MICRODRIVE_H
#define MICRODRIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    MICRODRIVE_COUNT = 2,
    MICRODRIVE_SECTOR_SIZE = 543,
    MICRODRIVE_MIN_SECTORS = 10,
    MICRODRIVE_MAX_SECTORS = 254
};

typedef struct MicrodriveCartridge {
    uint8_t *data;
    size_t data_size;
    bool inserted;
    bool dirty;
    bool write_protected;
    char path[260];
} MicrodriveCartridge;

typedef struct MicrodriveBank {
    MicrodriveCartridge drives[MICRODRIVE_COUNT];
} MicrodriveBank;

void microdrive_bank_init(MicrodriveBank *bank);
void microdrive_bank_discard(MicrodriveBank *bank);

bool microdrive_load_file(
    MicrodriveBank *bank,
    uint8_t drive,
    const char *path,
    char *error_buffer,
    size_t error_buffer_size
);

/* Creates a formatted, empty 254-sector MDR cartridge and inserts it. */
bool microdrive_create_file(
    MicrodriveBank *bank,
    uint8_t drive,
    const char *path,
    char *error_buffer,
    size_t error_buffer_size
);

bool microdrive_save(
    MicrodriveBank *bank,
    uint8_t drive,
    char *error_buffer,
    size_t error_buffer_size
);

void microdrive_eject(MicrodriveBank *bank, uint8_t drive);
int microdrive_first_empty(const MicrodriveBank *bank);
bool microdrive_any_dirty(const MicrodriveBank *bank);

bool microdrive_ready(void *user_data, uint8_t drive);
bool microdrive_write_protected(void *user_data, uint8_t drive);
uint32_t microdrive_length(void *user_data, uint8_t drive);
uint8_t microdrive_read_byte(void *user_data, uint8_t drive, uint32_t offset);
void microdrive_write_byte(void *user_data, uint8_t drive, uint32_t offset, uint8_t value);

#endif
