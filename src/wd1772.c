#include "wd1772.h"

#include <string.h>

enum {
    WD1772_STATUS_BUSY = 0x01,
    WD1772_STATUS_DRQ = 0x02,
    WD1772_STATUS_RECORD_NOT_FOUND = 0x10,
    WD1772_STATUS_WRITE_PROTECT = 0x40,
    WD1772_STATUS_NOT_READY = 0x80,
    WD1772_COMMAND_RESTORE = 0x00,
    WD1772_COMMAND_SEEK = 0x10,
    WD1772_COMMAND_STEP = 0x20,
    WD1772_COMMAND_STEP_IN = 0x40,
    WD1772_COMMAND_STEP_OUT = 0x60,
    WD1772_COMMAND_READ_SECTOR = 0x80,
    WD1772_COMMAND_WRITE_SECTOR = 0xA0,
    WD1772_COMMAND_FORCE_INTERRUPT = 0xD0,
    WD1772_REGISTER_STATUS_COMMAND = 0,
    WD1772_REGISTER_TRACK = 1,
    WD1772_REGISTER_SECTOR = 2,
    WD1772_REGISTER_DATA = 3
};

static void wd1772_finish(Wd1772 *fdc, uint8_t status) {
    fdc->status = status;
    fdc->transfer_size = 0;
    fdc->transfer_pos = 0;
    fdc->transfer_write = false;
}

void wd1772_init(Wd1772 *fdc, Wd1772ReadSector read_sector,
                 Wd1772WriteSector write_sector, void *user_data) {
    if (fdc == NULL) return;
    memset(fdc, 0, sizeof(*fdc));
    fdc->read_sector = read_sector;
    fdc->write_sector = write_sector;
    fdc->user_data = user_data;
    wd1772_reset(fdc);
}

void wd1772_reset(Wd1772 *fdc) {
    if (fdc == NULL) return;
    fdc->status = WD1772_STATUS_NOT_READY;
    fdc->track = 0;
    fdc->sector = 1;
    fdc->data = 0;
    fdc->step_direction_in = false;
    fdc->transfer_size = 0;
    fdc->transfer_pos = 0;
}

void wd1772_set_drive(Wd1772 *fdc, uint8_t drive, uint8_t side) {
    if (fdc != NULL) {
        fdc->drive = drive & 1;
        fdc->side = side & 1;
    }
}

static void wd1772_command(Wd1772 *fdc, uint8_t command) {
    const uint8_t operation = command & 0xF0;
    fdc->command = command;
    if (operation == WD1772_COMMAND_FORCE_INTERRUPT) {
        wd1772_finish(fdc, 0);
        return;
    }
    if (operation == WD1772_COMMAND_RESTORE) {
        fdc->track = 0;
        wd1772_finish(fdc, 0);
        return;
    }
    if (operation == WD1772_COMMAND_SEEK) {
        fdc->track = fdc->data;
        wd1772_finish(fdc, 0);
        return;
    }
    if (operation == WD1772_COMMAND_STEP || operation == WD1772_COMMAND_STEP_IN ||
        operation == WD1772_COMMAND_STEP_OUT) {
        if (operation == WD1772_COMMAND_STEP_IN) fdc->step_direction_in = true;
        if (operation == WD1772_COMMAND_STEP_OUT) fdc->step_direction_in = false;
        if (fdc->step_direction_in && fdc->track < 255) ++fdc->track;
        if (!fdc->step_direction_in && fdc->track > 0) --fdc->track;
        wd1772_finish(fdc, 0);
        return;
    }
    if (operation == WD1772_COMMAND_READ_SECTOR) {
        uint32_t size = 0;
        fdc->status = WD1772_STATUS_BUSY;
        if (fdc->read_sector == NULL || !fdc->read_sector(
                fdc->user_data, fdc->drive, fdc->track, fdc->side,
                fdc->sector, fdc->transfer, sizeof(fdc->transfer), &size) ||
            size == 0 || size > sizeof(fdc->transfer)) {
            wd1772_finish(fdc, WD1772_STATUS_RECORD_NOT_FOUND);
            return;
        }
        fdc->transfer_size = size;
        fdc->transfer_pos = 0;
        fdc->transfer_write = false;
        fdc->status = WD1772_STATUS_BUSY | WD1772_STATUS_DRQ;
        return;
    }
    if (operation == WD1772_COMMAND_WRITE_SECTOR) {
        uint32_t size = 0;
        if (fdc->read_sector == NULL || !fdc->read_sector(
                fdc->user_data, fdc->drive, fdc->track, fdc->side,
                fdc->sector, fdc->transfer, sizeof(fdc->transfer), &size) ||
            size == 0 || size > sizeof(fdc->transfer)) {
            wd1772_finish(fdc, WD1772_STATUS_RECORD_NOT_FOUND);
            return;
        }
        fdc->transfer_size = size;
        fdc->transfer_pos = 0;
        fdc->transfer_write = true;
        fdc->status = WD1772_STATUS_BUSY | WD1772_STATUS_DRQ;
        return;
    }
    wd1772_finish(fdc, WD1772_STATUS_RECORD_NOT_FOUND);
}

uint8_t wd1772_read_register(Wd1772 *fdc, uint8_t register_index) {
    if (fdc == NULL) return 0xFF;
    switch (register_index & 3) {
        case WD1772_REGISTER_STATUS_COMMAND: return fdc->status;
        case WD1772_REGISTER_TRACK: return fdc->track;
        case WD1772_REGISTER_SECTOR: return fdc->sector;
        default:
            if (!fdc->transfer_write && fdc->transfer_pos < fdc->transfer_size) {
                fdc->data = fdc->transfer[fdc->transfer_pos++];
                if (fdc->transfer_pos == fdc->transfer_size) wd1772_finish(fdc, 0);
            }
            return fdc->data;
    }
}

void wd1772_write_register(Wd1772 *fdc, uint8_t register_index, uint8_t value) {
    if (fdc == NULL) return;
    switch (register_index & 3) {
        case WD1772_REGISTER_STATUS_COMMAND:
            wd1772_command(fdc, value);
            break;
        case WD1772_REGISTER_TRACK:
            fdc->track = value;
            break;
        case WD1772_REGISTER_SECTOR:
            fdc->sector = value;
            break;
        default:
            fdc->data = value;
            if (fdc->transfer_write && fdc->transfer_pos < fdc->transfer_size) {
                fdc->transfer[fdc->transfer_pos++] = value;
                if (fdc->transfer_pos == fdc->transfer_size) {
                    const bool ok = fdc->write_sector != NULL && fdc->write_sector(
                        fdc->user_data, fdc->drive, fdc->track, fdc->side,
                        fdc->sector, fdc->transfer, fdc->transfer_size);
                    wd1772_finish(fdc, ok ? 0 : WD1772_STATUS_WRITE_PROTECT);
                }
            }
            break;
    }
}
