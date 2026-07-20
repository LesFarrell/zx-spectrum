#include "microdrive.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void microdrive_bank_init(MicrodriveBank *bank) {
    if (bank != NULL) {
        memset(bank, 0, sizeof(*bank));
    }
}

void microdrive_eject(MicrodriveBank *bank, uint8_t drive) {
    MicrodriveCartridge *cartridge;

    if (bank == NULL || drive >= MICRODRIVE_COUNT) {
        return;
    }
    cartridge = &bank->drives[drive];
    free(cartridge->data);
    memset(cartridge, 0, sizeof(*cartridge));
}

void microdrive_bank_discard(MicrodriveBank *bank) {
    if (bank == NULL) {
        return;
    }
    for (uint8_t drive = 0; drive < MICRODRIVE_COUNT; ++drive) {
        microdrive_eject(bank, drive);
    }
}

static bool microdrive_read_file(
    const char *path,
    uint8_t **out_data,
    size_t *out_size,
    char *error_buffer,
    size_t error_buffer_size)
{
    FILE *file;
    uint8_t *data;
    long file_size;

    file = fopen(path, "rb");
    if (file == NULL) {
        snprintf(error_buffer, error_buffer_size, "Could not open Microdrive cartridge: %s", path);
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) < 0) {
        fclose(file);
        snprintf(error_buffer, error_buffer_size, "Could not determine Microdrive cartridge size: %s", path);
        return false;
    }
    rewind(file);

    data = (uint8_t *)malloc((size_t)file_size);
    if (data == NULL) {
        fclose(file);
        snprintf(error_buffer, error_buffer_size, "Out of memory while loading Microdrive cartridge.");
        return false;
    }
    if (fread(data, 1, (size_t)file_size, file) != (size_t)file_size) {
        free(data);
        fclose(file);
        snprintf(error_buffer, error_buffer_size, "Could not read Microdrive cartridge: %s", path);
        return false;
    }
    fclose(file);
    *out_data = data;
    *out_size = (size_t)file_size;
    return true;
}

bool microdrive_load_file(
    MicrodriveBank *bank,
    uint8_t drive,
    const char *path,
    char *error_buffer,
    size_t error_buffer_size)
{
    MicrodriveCartridge loaded;
    size_t sector_bytes;

    if (bank == NULL || drive >= MICRODRIVE_COUNT || path == NULL) {
        snprintf(error_buffer, error_buffer_size, "Invalid Microdrive drive or path.");
        return false;
    }

    memset(&loaded, 0, sizeof(loaded));
    if (!microdrive_read_file(path, &loaded.data, &loaded.data_size, error_buffer, error_buffer_size)) {
        return false;
    }

    sector_bytes = loaded.data_size - (loaded.data_size % MICRODRIVE_SECTOR_SIZE);
    if ((loaded.data_size % MICRODRIVE_SECTOR_SIZE) > 1 ||
        sector_bytes < MICRODRIVE_MIN_SECTORS * MICRODRIVE_SECTOR_SIZE ||
        sector_bytes > MICRODRIVE_MAX_SECTORS * MICRODRIVE_SECTOR_SIZE) {
        free(loaded.data);
        snprintf(
            error_buffer,
            error_buffer_size,
            "Invalid MDR image. Expected 10-254 sectors of 543 bytes, with an optional write-protect byte.");
        return false;
    }

    loaded.write_protected =
        loaded.data_size == sector_bytes + 1 && loaded.data[sector_bytes] != 0;
    loaded.data_size = sector_bytes;
    loaded.inserted = true;
    snprintf(loaded.path, sizeof(loaded.path), "%s", path);

    microdrive_eject(bank, drive);
    bank->drives[drive] = loaded;
    return true;
}

static uint8_t microdrive_checksum(const uint8_t *data, size_t length) {
    uint16_t checksum = 0;

    for (size_t index = 0; index < length; ++index) {
        checksum += data[index];
        if (checksum == 0xFF) {
            checksum = 0;
        }
        else if (checksum > 0xFF) {
            checksum = (checksum & 0xFF) + 1;
        }
    }
    return (uint8_t)checksum;
}

static void microdrive_label_from_path(const char *path, uint8_t label[10]) {
    const char *name = path;
    const char *cursor;
    size_t length = 0;

    memset(label, ' ', 10);
    for (cursor = path; *cursor != '\0'; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
            name = cursor + 1;
        }
    }
    while (name[length] != '\0' && name[length] != '.' && length < 10) {
        const unsigned char value = (unsigned char)name[length];
        label[length] = (uint8_t)(
            value >= 32 && value <= 126 ? toupper(value) : '_');
        length++;
    }
    if (length == 0) {
        memcpy(label, "BLANK     ", 10);
    }
}

bool microdrive_create_file(
    MicrodriveBank *bank,
    uint8_t drive,
    const char *path,
    char *error_buffer,
    size_t error_buffer_size)
{
    MicrodriveCartridge created;
    uint8_t label[10];
    uint8_t write_protect = 0;
    FILE *file;

    if (bank == NULL || drive >= MICRODRIVE_COUNT || path == NULL || path[0] == '\0') {
        snprintf(error_buffer, error_buffer_size, "Invalid Microdrive drive or path.");
        return false;
    }

    memset(&created, 0, sizeof(created));
    created.data_size = MICRODRIVE_MAX_SECTORS * MICRODRIVE_SECTOR_SIZE;
    created.data = (uint8_t *)calloc(created.data_size, 1);
    if (created.data == NULL) {
        snprintf(error_buffer, error_buffer_size, "Out of memory while creating Microdrive cartridge.");
        return false;
    }

    microdrive_label_from_path(path, label);
    for (size_t sector_index = 0; sector_index < MICRODRIVE_MAX_SECTORS; ++sector_index) {
        uint8_t *sector = created.data + sector_index * MICRODRIVE_SECTOR_SIZE;
        sector[0] = 0x01;
        sector[1] = (uint8_t)(MICRODRIVE_MAX_SECTORS - sector_index);
        memcpy(sector + 4, label, sizeof(label));
        sector[14] = microdrive_checksum(sector, 14);
        /* A zero record header and zero data/checksum mark a free sector. */
    }

    file = fopen(path, "wb");
    if (file == NULL) {
        free(created.data);
        snprintf(error_buffer, error_buffer_size, "Could not create Microdrive cartridge: %s", path);
        return false;
    }
    {
        const bool write_failed =
            fwrite(created.data, 1, created.data_size, file) != created.data_size ||
            fwrite(&write_protect, 1, 1, file) != 1;
        const bool close_failed = fclose(file) != 0;
        if (write_failed || close_failed) {
            free(created.data);
            snprintf(error_buffer, error_buffer_size, "Could not finish creating Microdrive cartridge: %s", path);
            return false;
        }
    }

    created.inserted = true;
    snprintf(created.path, sizeof(created.path), "%s", path);
    microdrive_eject(bank, drive);
    bank->drives[drive] = created;
    return true;
}

bool microdrive_save(
    MicrodriveBank *bank,
    uint8_t drive,
    char *error_buffer,
    size_t error_buffer_size)
{
    MicrodriveCartridge *cartridge;
    FILE *file;
    uint8_t write_protect;

    if (bank == NULL || drive >= MICRODRIVE_COUNT || !bank->drives[drive].inserted) {
        snprintf(error_buffer, error_buffer_size, "No cartridge is inserted in that Microdrive.");
        return false;
    }
    cartridge = &bank->drives[drive];
    if (cartridge->path[0] == '\0') {
        snprintf(error_buffer, error_buffer_size, "The cartridge has no destination filename.");
        return false;
    }

    file = fopen(cartridge->path, "wb");
    if (file == NULL) {
        snprintf(error_buffer, error_buffer_size, "Could not save Microdrive cartridge: %s", cartridge->path);
        return false;
    }
    write_protect = cartridge->write_protected ? 1 : 0;
    {
        const bool write_failed =
            fwrite(cartridge->data, 1, cartridge->data_size, file) != cartridge->data_size ||
            fwrite(&write_protect, 1, 1, file) != 1;
        const bool close_failed = fclose(file) != 0;
        if (write_failed || close_failed) {
            snprintf(
                error_buffer,
                error_buffer_size,
                "Could not finish saving Microdrive cartridge: %s",
                cartridge->path);
            return false;
        }
    }
    cartridge->dirty = false;
    return true;
}

int microdrive_first_empty(const MicrodriveBank *bank) {
    if (bank == NULL) {
        return -1;
    }
    for (int drive = 0; drive < MICRODRIVE_COUNT; ++drive) {
        if (!bank->drives[drive].inserted) {
            return drive;
        }
    }
    return -1;
}

bool microdrive_any_dirty(const MicrodriveBank *bank) {
    if (bank == NULL) {
        return false;
    }
    for (uint8_t drive = 0; drive < MICRODRIVE_COUNT; ++drive) {
        if (bank->drives[drive].inserted && bank->drives[drive].dirty) {
            return true;
        }
    }
    return false;
}

bool microdrive_ready(void *user_data, uint8_t drive) {
    MicrodriveBank *bank = (MicrodriveBank *)user_data;
    return bank != NULL && drive < MICRODRIVE_COUNT && bank->drives[drive].inserted;
}

bool microdrive_write_protected(void *user_data, uint8_t drive) {
    MicrodriveBank *bank = (MicrodriveBank *)user_data;
    return bank == NULL || drive >= MICRODRIVE_COUNT ||
        !bank->drives[drive].inserted || bank->drives[drive].write_protected;
}

uint32_t microdrive_length(void *user_data, uint8_t drive) {
    MicrodriveBank *bank = (MicrodriveBank *)user_data;
    if (bank == NULL || drive >= MICRODRIVE_COUNT || !bank->drives[drive].inserted) {
        return 0;
    }
    return (uint32_t)bank->drives[drive].data_size;
}

uint8_t microdrive_read_byte(void *user_data, uint8_t drive, uint32_t offset) {
    MicrodriveBank *bank = (MicrodriveBank *)user_data;
    if (bank == NULL || drive >= MICRODRIVE_COUNT ||
        !bank->drives[drive].inserted || offset >= bank->drives[drive].data_size) {
        return 0xFF;
    }
    return bank->drives[drive].data[offset];
}

void microdrive_write_byte(void *user_data, uint8_t drive, uint32_t offset, uint8_t value) {
    MicrodriveBank *bank = (MicrodriveBank *)user_data;
    MicrodriveCartridge *cartridge;

    if (bank == NULL || drive >= MICRODRIVE_COUNT) {
        return;
    }
    cartridge = &bank->drives[drive];
    if (!cartridge->inserted || cartridge->write_protected || offset >= cartridge->data_size) {
        return;
    }
    if (cartridge->data[offset] != value) {
        cartridge->data[offset] = value;
        cartridge->dirty = true;
    }
}
