#ifndef WD1772_H
#define WD1772_H

#include <stdbool.h>
#include <stdint.h>

typedef bool (*Wd1772ReadSector)(
    void *user_data,
    uint8_t drive,
    uint8_t track,
    uint8_t side,
    uint8_t sector,
    uint8_t *buffer,
    uint32_t buffer_size,
    uint32_t *data_size);
typedef bool (*Wd1772WriteSector)(
    void *user_data,
    uint8_t drive,
    uint8_t track,
    uint8_t side,
    uint8_t sector,
    const uint8_t *buffer,
    uint32_t data_size);

typedef struct Wd1772 {
    uint8_t status;
    uint8_t track;
    uint8_t sector;
    uint8_t data;
    uint8_t command;
    uint8_t drive;
    uint8_t side;
    bool step_direction_in;
    uint8_t transfer[8192];
    uint32_t transfer_size;
    uint32_t transfer_pos;
    bool transfer_write;
    Wd1772ReadSector read_sector;
    Wd1772WriteSector write_sector;
    void *user_data;
} Wd1772;

void wd1772_init(
    Wd1772 *fdc,
    Wd1772ReadSector read_sector,
    Wd1772WriteSector write_sector,
    void *user_data);
void wd1772_reset(Wd1772 *fdc);
void wd1772_set_drive(Wd1772 *fdc, uint8_t drive, uint8_t side);
uint8_t wd1772_read_register(Wd1772 *fdc, uint8_t register_index);
void wd1772_write_register(Wd1772 *fdc, uint8_t register_index, uint8_t value);

#endif
