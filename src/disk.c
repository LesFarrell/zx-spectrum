#include "disk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    DSK_HEADER_SIZE = 0x100,
    DSK_TRACK_HEADER_SIZE = 0x100,
    DSK_MAX_SIDES = 2,
    DSK_MAX_IMAGE_SIZE = 64 * 1024 * 1024
};

static uint16_t dsk_read_u16(const uint8_t *data) {
    return (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
}

static uint32_t dsk_size_from_code(uint8_t n) {
    if (n > 7) {
        return 0;
    }
    return 128u << n;
}

void dsk_init(DskImage *image) {
    if (image != NULL) {
        memset(image, 0, sizeof(*image));
    }
}

void dsk_discard(DskImage *image) {
    if (image == NULL) {
        return;
    }
    if (image->tracks != NULL) {
        for (size_t index = 0; index < image->track_slot_count; ++index) {
            free(image->tracks[index].sectors);
        }
    }
    free(image->tracks);
    free(image->data);
    dsk_init(image);
}

static DskTrack *dsk_find_track(DskImage *image, uint8_t c, uint8_t h) {
    if (image == NULL || !image->inserted) {
        return NULL;
    }
    for (size_t index = 0; index < image->track_slot_count; ++index) {
        DskTrack *track = &image->tracks[index];
        if (track->sectors != NULL && track->c == c && track->h == h) {
            return track;
        }
    }
    return NULL;
}

static DskSector *dsk_find_sector(
    DskImage *image,
    uint8_t c,
    uint8_t h,
    uint8_t r,
    uint8_t n)
{
    DskTrack *track = dsk_find_track(image, c, h);
    if (track == NULL) {
        return NULL;
    }
    for (uint8_t index = 0; index < track->sector_count; ++index) {
        DskSector *sector = &track->sectors[index];
        if (sector->c == c && sector->h == h && sector->r == r &&
            (sector->n == n || n == 0xFF)) {
            return sector;
        }
    }
    return NULL;
}

static bool dsk_parse_owned_data(
    DskImage *image,
    char *error_buffer,
    size_t error_buffer_size)
{
    const uint8_t *header = image->data;
    uint32_t standard_track_size = 0;
    size_t file_offset = DSK_HEADER_SIZE;

    if (image->data_size < DSK_HEADER_SIZE) {
        snprintf(error_buffer, error_buffer_size, "DSK image is smaller than its 256-byte header.");
        return false;
    }
    if (memcmp(header, "MV - CPCEMU Disk-File\r\nDisk-Info\r\n", 34) == 0) {
        image->extended = false;
        standard_track_size = dsk_read_u16(header + 0x32);
    }
    else if (memcmp(header, "EXTENDED CPC DSK File\r\nDisk-Info\r\n", 34) == 0) {
        image->extended = true;
    }
    else {
        snprintf(error_buffer, error_buffer_size, "File is not a standard or extended CPC/+3 DSK image.");
        return false;
    }

    image->cylinder_count = header[0x30];
    image->side_count = header[0x31];
    if (image->cylinder_count == 0 ||
        image->side_count == 0 || image->side_count > DSK_MAX_SIDES) {
        snprintf(
            error_buffer,
            error_buffer_size,
            "DSK image has an unsupported geometry (%u tracks, %u sides).",
            image->cylinder_count,
            image->side_count);
        return false;
    }

    image->track_slot_count = (size_t)image->cylinder_count * image->side_count;
    if (image->extended && 0x34u + image->track_slot_count > DSK_HEADER_SIZE) {
        snprintf(error_buffer, error_buffer_size, "Extended DSK track-size table is too large.");
        return false;
    }
    if (!image->extended && standard_track_size < DSK_TRACK_HEADER_SIZE) {
        snprintf(error_buffer, error_buffer_size, "Standard DSK track size is invalid.");
        return false;
    }

    image->tracks = (DskTrack *)calloc(image->track_slot_count, sizeof(*image->tracks));
    if (image->tracks == NULL) {
        snprintf(error_buffer, error_buffer_size, "Out of memory while indexing DSK tracks.");
        return false;
    }

    for (size_t track_index = 0; track_index < image->track_slot_count; ++track_index) {
        DskTrack *track = &image->tracks[track_index];
        uint32_t track_size = image->extended
            ? (uint32_t)header[0x34 + track_index] << 8
            : standard_track_size;
        size_t sector_data_offset;

        if (track_size == 0) {
            continue;
        }
        if (track_size < DSK_TRACK_HEADER_SIZE ||
            file_offset > image->data_size ||
            track_size > image->data_size - file_offset) {
            snprintf(error_buffer, error_buffer_size, "DSK track %zu extends beyond the file.", track_index);
            return false;
        }
        if (memcmp(image->data + file_offset, "Track-Info\r\n", 12) != 0) {
            snprintf(error_buffer, error_buffer_size, "DSK track %zu has no Track-Info header.", track_index);
            return false;
        }

        track->c = image->data[file_offset + 0x10];
        track->h = image->data[file_offset + 0x11];
        track->sector_count = image->data[file_offset + 0x15];
        if ((size_t)0x18 + ((size_t)track->sector_count * 8u) > DSK_TRACK_HEADER_SIZE) {
            snprintf(error_buffer, error_buffer_size, "DSK track %zu has too many sector descriptors.", track_index);
            return false;
        }

        if (track->sector_count > 0) {
            track->sectors = (DskSector *)calloc(track->sector_count, sizeof(*track->sectors));
            if (track->sectors == NULL) {
                snprintf(error_buffer, error_buffer_size, "Out of memory while indexing DSK sectors.");
                return false;
            }
        }
        sector_data_offset = file_offset + DSK_TRACK_HEADER_SIZE;

        for (uint8_t sector_index = 0; sector_index < track->sector_count; ++sector_index) {
            const uint8_t *descriptor =
                image->data + file_offset + 0x18 + ((size_t)sector_index * 8u);
            DskSector *sector = &track->sectors[sector_index];
            const uint32_t nominal_size = dsk_size_from_code(descriptor[3]);
            uint32_t stored_size = image->extended
                ? dsk_read_u16(descriptor + 6)
                : nominal_size;

            if (!image->extended && stored_size == 0) {
                stored_size = nominal_size;
            }
            if (sector_data_offset > file_offset + track_size ||
                stored_size > (file_offset + track_size) - sector_data_offset) {
                snprintf(
                    error_buffer,
                    error_buffer_size,
                    "DSK track %zu sector %u has an invalid data length.",
                    track_index,
                    sector_index);
                return false;
            }

            sector->c = descriptor[0];
            sector->h = descriptor[1];
            sector->r = descriptor[2];
            sector->n = descriptor[3];
            sector->st1 = descriptor[4];
            sector->st2 = descriptor[5];
            sector->stored_size = stored_size;
            sector->size =
                stored_size != 0 && nominal_size != 0 && stored_size > nominal_size &&
                (stored_size % nominal_size) == 0
                    ? nominal_size
                    : stored_size;
            sector->data = stored_size != 0
                ? image->data + sector_data_offset
                : NULL;
            sector_data_offset += stored_size;
        }
        file_offset += track_size;
    }

    if (file_offset > image->data_size) {
        snprintf(error_buffer, error_buffer_size, "DSK track data is truncated.");
        return false;
    }
    image->inserted = true;
    return true;
}

bool dsk_load_data(
    DskImage *image,
    const uint8_t *data,
    size_t data_size,
    char *error_buffer,
    size_t error_buffer_size)
{
    DskImage loaded;
    dsk_init(&loaded);

    if (image == NULL || data == NULL || data_size == 0) {
        snprintf(error_buffer, error_buffer_size, "No DSK image data was provided.");
        return false;
    }
    if (data_size > DSK_MAX_IMAGE_SIZE) {
        snprintf(error_buffer, error_buffer_size, "DSK image exceeds the 64 MB safety limit.");
        return false;
    }
    loaded.data = (uint8_t *)malloc(data_size);
    if (loaded.data == NULL) {
        snprintf(error_buffer, error_buffer_size, "Out of memory while loading DSK image.");
        return false;
    }
    memcpy(loaded.data, data, data_size);
    loaded.data_size = data_size;

    if (!dsk_parse_owned_data(&loaded, error_buffer, error_buffer_size)) {
        dsk_discard(&loaded);
        return false;
    }

    dsk_discard(image);
    *image = loaded;
    return true;
}

bool dsk_load_file(
    DskImage *image,
    const char *path,
    char *error_buffer,
    size_t error_buffer_size)
{
    FILE *file;
    uint8_t *data;
    long file_size;
    bool loaded;

    if (path == NULL || path[0] == '\0') {
        snprintf(error_buffer, error_buffer_size, "No DSK path was provided.");
        return false;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        snprintf(error_buffer, error_buffer_size, "Could not open DSK image: %s", path);
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) <= 0) {
        fclose(file);
        snprintf(error_buffer, error_buffer_size, "Could not determine DSK image size: %s", path);
        return false;
    }
    if ((unsigned long)file_size > DSK_MAX_IMAGE_SIZE) {
        fclose(file);
        snprintf(error_buffer, error_buffer_size, "DSK image exceeds the 64 MB safety limit: %s", path);
        return false;
    }
    rewind(file);
    data = (uint8_t *)malloc((size_t)file_size);
    if (data == NULL) {
        fclose(file);
        snprintf(error_buffer, error_buffer_size, "Out of memory while reading DSK image.");
        return false;
    }
    if (fread(data, 1, (size_t)file_size, file) != (size_t)file_size) {
        free(data);
        fclose(file);
        snprintf(error_buffer, error_buffer_size, "Could not read DSK image: %s", path);
        return false;
    }
    fclose(file);

    loaded = dsk_load_data(image, data, (size_t)file_size, error_buffer, error_buffer_size);
    free(data);
    if (loaded) {
        snprintf(image->path, sizeof(image->path), "%s", path);
    }
    return loaded;
}

bool dsk_drive_ready(void *user_data, uint8_t drive) {
    DskImage *image = (DskImage *)user_data;
    return drive == 0 && image != NULL && image->inserted;
}

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
    uint8_t *st2)
{
    DskImage *image = (DskImage *)user_data;
    DskSector *sector;
    if (!dsk_drive_ready(image, drive) || buffer == NULL || data_size == NULL) {
        return false;
    }
    sector = dsk_find_sector(image, c, h, r, n);
    if (sector == NULL || sector->size == 0 || sector->data == NULL ||
        sector->size > buffer_size) {
        return false;
    }
    memcpy(buffer, sector->data, sector->size);
    *data_size = sector->size;
    if (st1 != NULL) {
        *st1 = sector->st1;
    }
    if (st2 != NULL) {
        *st2 = sector->st2;
    }
    return true;
}

bool dsk_write_sector(
    void *user_data,
    uint8_t drive,
    uint8_t c,
    uint8_t h,
    uint8_t r,
    uint8_t n,
    const uint8_t *buffer,
    uint32_t data_size)
{
    DskImage *image = (DskImage *)user_data;
    DskSector *sector;
    if (!dsk_drive_ready(image, drive) || buffer == NULL) {
        return false;
    }
    sector = dsk_find_sector(image, c, h, r, n);
    if (sector == NULL || sector->size == 0 || sector->data == NULL ||
        sector->size != data_size) {
        return false;
    }
    for (uint32_t offset = 0; offset + data_size <= sector->stored_size; offset += data_size) {
        memcpy(sector->data + offset, buffer, data_size);
    }
    sector->st1 = 0;
    sector->st2 = 0;
    image->dirty = true;
    return true;
}

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
    uint8_t *st2)
{
    DskImage *image = (DskImage *)user_data;
    DskTrack *track;
    DskSector *sector;
    if (!dsk_drive_ready(image, drive)) {
        return false;
    }
    track = dsk_find_track(image, c, h);
    if (track == NULL || track->sector_count == 0) {
        return false;
    }
    sector = &track->sectors[index % track->sector_count];
    if (out_c != NULL) {
        *out_c = sector->c;
    }
    if (out_h != NULL) {
        *out_h = sector->h;
    }
    if (out_r != NULL) {
        *out_r = sector->r;
    }
    if (out_n != NULL) {
        *out_n = sector->n;
    }
    if (st1 != NULL) {
        *st1 = sector->st1;
    }
    if (st2 != NULL) {
        *st2 = sector->st2;
    }
    return true;
}
