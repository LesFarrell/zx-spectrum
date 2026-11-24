#include "disk.h"
#include "file_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    DSK_HEADER_SIZE = 0x100,
    DSK_TRACK_HEADER_SIZE = 0x100,
    DSK_MAX_SIDES = 2,
    DSK_MAX_IMAGE_SIZE = 64 * 1024 * 1024,
    MGT_TRACK_SIZE = 10 * 512,
    MGT_40S_SIZE = 40 * MGT_TRACK_SIZE,
    MGT_40D_SIZE = 40 * 2 * MGT_TRACK_SIZE,
    MGT_80D_SIZE = 80 * 2 * MGT_TRACK_SIZE,
    TRD_TRACK_SIZE = 16 * 256,
    TRD_IMAGE_SIZE = 80 * 2 * TRD_TRACK_SIZE,
    OPD_TRACK_SIZE = 18 * 256,
    OPD_40S_SIZE = 40 * OPD_TRACK_SIZE,
    OPD_40D_SIZE = 40 * 2 * OPD_TRACK_SIZE
    ,SDF_HEADER_SIZE = 512
    ,SDF_TRACK_RECORD_SIZE = 6656
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

static bool dsk_load_data_ordered(
    DskImage *image,
    const uint8_t *data,
    size_t data_size,
    char *error_buffer,
    size_t error_buffer_size,
    bool raw_side_contiguous);

static bool dsk_parse_raw_data(
    DskImage *image,
    uint8_t cylinders,
    uint8_t sides,
    uint8_t sectors_per_track,
    uint16_t sector_size,
    size_t track_size,
    char *error_buffer,
    size_t error_buffer_size) {
    const size_t track_count = (size_t)cylinders * sides;

    image->extended = false;
    image->cylinder_count = cylinders;
    image->side_count = sides;
    image->track_slot_count = track_count;
    image->tracks = (DskTrack *)calloc(track_count, sizeof(*image->tracks));
    if (image->tracks == NULL) {
        snprintf(error_buffer, error_buffer_size, "Out of memory while indexing MGT tracks.");
        return false;
    }
    for (size_t track_index = 0; track_index < track_count; ++track_index) {
        DskTrack *track = &image->tracks[track_index];
        if (image->raw_side_contiguous) {
            track->h = (uint8_t)(track_index / cylinders);
            track->c = (uint8_t)(track_index % cylinders);
        } else {
            track->c = (uint8_t)(track_index / sides);
            track->h = (uint8_t)(track_index % sides);
        }
        track->sector_count = sectors_per_track;
        track->sectors = (DskSector *)calloc(track->sector_count, sizeof(*track->sectors));
        if (track->sectors == NULL) {
            snprintf(error_buffer, error_buffer_size, "Out of memory while indexing MGT sectors.");
            return false;
        }
        for (uint8_t sector_index = 0; sector_index < track->sector_count; ++sector_index) {
            DskSector *sector = &track->sectors[sector_index];
            sector->c = track->c;
            sector->h = track->h;
            sector->r = (uint8_t)(sector_index + 1);
            uint8_t size_code = 0;
            while (((uint32_t)128u << size_code) < sector_size && size_code < 7) {
                ++size_code;
            }
            sector->n = size_code;
            sector->size = sector_size;
            sector->stored_size = sector_size;
            size_t raw_track_index = image->raw_side_contiguous
                ? ((size_t)track->h * cylinders) + track->c
                : track_index;
            sector->data = image->data + (raw_track_index * track_size) +
                ((size_t)sector_index * sector_size);
        }
    }
    image->inserted = true;
    return true;
}

static bool dsk_parse_mgt_data(
    DskImage *image,
    char *error_buffer,
    size_t error_buffer_size) {
    const uint8_t cylinders = image->data_size == MGT_80D_SIZE ? 80 : 40;
    const uint8_t sides = image->data_size == MGT_40S_SIZE ? 1 : 2;
    return dsk_parse_raw_data(
        image, cylinders, sides, 10, 512, MGT_TRACK_SIZE,
        error_buffer, error_buffer_size);
}

static bool dsk_parse_trd_data(DskImage *image, char *error_buffer, size_t error_buffer_size) {
    return dsk_parse_raw_data(
        image, 80, 2, 16, 256, TRD_TRACK_SIZE,
        error_buffer, error_buffer_size);
}

static bool dsk_parse_opd_data(DskImage *image, char *error_buffer, size_t error_buffer_size) {
    const uint8_t sides = image->data_size == OPD_40S_SIZE ? 1 : 2;
    return dsk_parse_raw_data(
        image, 40, sides, 18, 256, OPD_TRACK_SIZE,
        error_buffer, error_buffer_size);
}

static bool dsk_parse_sdf_data(DskImage *image, char *error_buffer, size_t error_buffer_size) {
    const uint8_t cylinders = image->data[4];
    const uint8_t sides = image->data[5];
    const size_t track_count = (size_t)cylinders * sides;

    if (cylinders == 0 || cylinders > 80 || (sides != 1 && sides != 2) ||
        image->data_size < SDF_HEADER_SIZE ||
        (image->data_size - SDF_HEADER_SIZE) / SDF_TRACK_RECORD_SIZE != track_count ||
        (image->data_size - SDF_HEADER_SIZE) % SDF_TRACK_RECORD_SIZE != 0) {
        snprintf(error_buffer, error_buffer_size, "SDF image has invalid geometry or track records.");
        return false;
    }
    image->extended = true;
    image->cylinder_count = cylinders;
    image->side_count = sides;
    image->track_slot_count = track_count;
    image->source_read_only = image->data[6] == 0xFF;
    image->tracks = (DskTrack *)calloc(track_count, sizeof(*image->tracks));
    if (image->tracks == NULL) {
        snprintf(error_buffer, error_buffer_size, "Out of memory while indexing SDF tracks.");
        return false;
    }
    for (size_t track_index = 0; track_index < track_count; ++track_index) {
        const size_t record_offset = SDF_HEADER_SIZE + track_index * SDF_TRACK_RECORD_SIZE;
        const uint8_t *record = image->data + record_offset;
        DskTrack *track = &image->tracks[track_index];
        const uint8_t sector_count = record[0];
        if (sector_count > 31) {
            snprintf(error_buffer, error_buffer_size, "SDF track %zu has too many sectors.", track_index);
            return false;
        }
        track->c = (uint8_t)(track_index / sides);
        track->h = (uint8_t)(track_index % sides);
        track->sector_count = sector_count;
        if (sector_count != 0) {
            track->sectors = (DskSector *)calloc(sector_count, sizeof(*track->sectors));
            if (track->sectors == NULL) {
                snprintf(error_buffer, error_buffer_size, "Out of memory while indexing SDF sectors.");
                return false;
            }
        }
        for (uint8_t sector_index = 0; sector_index < sector_count; ++sector_index) {
            const uint8_t *entry = record + 8 + ((size_t)sector_index * 8u);
            const uint16_t id_offset = (uint16_t)(entry[0] | ((uint16_t)entry[1] << 8)) & 0x3FFF;
            const uint16_t data_offset = (uint16_t)(entry[2] | ((uint16_t)entry[3] << 8)) & 0x3FFF;
            const uint8_t size_code = entry[7];
            const uint32_t sector_size = dsk_size_from_code(size_code);
            DskSector *sector = &track->sectors[sector_index];
            if (sector_size == 0 || id_offset >= SDF_TRACK_RECORD_SIZE ||
                data_offset >= SDF_TRACK_RECORD_SIZE ||
                (size_t)data_offset + sector_size > SDF_TRACK_RECORD_SIZE) {
                snprintf(error_buffer, error_buffer_size, "SDF track %zu has an invalid sector.", track_index);
                return false;
            }
            sector->c = entry[4];
            sector->h = entry[5];
            sector->r = entry[6];
            sector->n = size_code;
            sector->size = sector_size;
            sector->stored_size = sector_size;
            sector->data = image->data + record_offset + data_offset;
        }
    }
    image->inserted = true;
    return true;
}

static bool dsk_parse_fdi_data(DskImage *image, char *error_buffer, size_t error_buffer_size) {
    const uint16_t cylinders = (uint16_t)(image->data[4] | ((uint16_t)image->data[5] << 8));
    const uint16_t sides = (uint16_t)(image->data[6] | ((uint16_t)image->data[7] << 8));
    const uint16_t info_size = (uint16_t)(image->data[12] | ((uint16_t)image->data[13] << 8));
    const size_t track_headers = 0x0E + info_size;
    const uint32_t data_offset = image->data[10] | ((uint32_t)image->data[11] << 8);
    const size_t track_count = (size_t)cylinders * sides;

    if (cylinders == 0 || cylinders > 255 || sides == 0 || sides > 2 ||
        track_headers > image->data_size || data_offset > image->data_size ||
        track_count > (image->data_size - track_headers) / 7u) {
        snprintf(error_buffer, error_buffer_size, "FDI image has invalid geometry or track tables.");
        return false;
    }
    image->extended = true;
    image->cylinder_count = (uint8_t)cylinders;
    image->side_count = (uint8_t)sides;
    image->track_slot_count = track_count;
    image->source_read_only = image->data[3] != 0;
    image->tracks = (DskTrack *)calloc(track_count, sizeof(*image->tracks));
    if (image->tracks == NULL) {
        snprintf(error_buffer, error_buffer_size, "Out of memory while indexing FDI tracks.");
        return false;
    }
    size_t header_offset = track_headers;
    for (size_t track_index = 0; track_index < track_count; ++track_index) {
        const uint8_t *header;
        uint32_t track_offset;
        uint8_t sector_count;
        size_t sector_table;
        DskTrack *track = &image->tracks[track_index];
        header = header_offset <= image->data_size ? image->data + header_offset : NULL;
        if (header == NULL) {
            snprintf(error_buffer, error_buffer_size, "FDI track %zu header is truncated.", track_index);
            return false;
        }
        track_offset = header[0] | ((uint32_t)header[1] << 8) |
            ((uint32_t)header[2] << 16) | ((uint32_t)header[3] << 24);
        sector_count = header[6];
        sector_table = header_offset + 7u;
        if (sector_count > 35 || sector_table > image->data_size ||
            (size_t)sector_count * 7u > image->data_size - sector_table) {
            snprintf(error_buffer, error_buffer_size, "FDI track %zu has an invalid sector table.", track_index);
            return false;
        }
        track->c = (uint8_t)(track_index / sides);
        track->h = (uint8_t)(track_index % sides);
        track->sector_count = sector_count;
        track->sectors = sector_count == 0 ? NULL :
            (DskSector *)calloc(sector_count, sizeof(*track->sectors));
        if (sector_count != 0 && track->sectors == NULL) {
            snprintf(error_buffer, error_buffer_size, "Out of memory while indexing FDI sectors.");
            return false;
        }
        header_offset = sector_table + ((size_t)sector_count * 7u);
        for (uint8_t sector_index = 0; sector_index < sector_count; ++sector_index) {
            const uint8_t *entry = image->data + sector_table + ((size_t)sector_index * 7u);
            const uint16_t sector_offset = (uint16_t)(entry[5] | ((uint16_t)entry[6] << 8));
            const uint32_t sector_size = dsk_size_from_code(entry[3]);
            DskSector *sector = &track->sectors[sector_index];
            if (sector_size == 0 ||
                data_offset > image->data_size ||
                (uint64_t)data_offset + track_offset + sector_offset + sector_size > image->data_size) {
                snprintf(error_buffer, error_buffer_size, "FDI track %zu has an invalid sector.", track_index);
                return false;
            }
            sector->c = entry[0];
            sector->h = entry[1];
            sector->r = entry[2];
            sector->n = entry[3];
            sector->st2 = (entry[4] & 0x80) != 0 ? 0x40 : 0;
            sector->size = sector_size;
            sector->stored_size = sector_size;
            sector->data = image->data + data_offset + track_offset + sector_offset;
        }
    }
    image->inserted = true;
    return true;
}

static uint32_t dsk_read_u32(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
        ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static bool dsk_td0_decode_sector(
    const uint8_t *encoded,
    size_t encoded_size,
    uint8_t encoding,
    uint8_t *destination,
    size_t destination_size)
{
    size_t source = 0;
    size_t output = 0;
    if (encoding == 0) {
        if (encoded_size < destination_size) return false;
        memcpy(destination, encoded, destination_size);
        return true;
    }
    if (encoding == 1) {
        if (encoded_size < 4 || dsk_read_u16(encoded) != destination_size) return false;
        const uint16_t count = dsk_read_u16(encoded + 2);
        if (count > destination_size / 2 || 4u + 2u > encoded_size) return false;
        while (output + 2 <= destination_size) {
            destination[output++] = encoded[4];
            destination[output++] = encoded[5];
        }
        return count == destination_size / 2;
    }
    if (encoding != 2) return false;
    while (output < destination_size) {
        uint8_t count;
        if (source >= encoded_size) return false;
        count = encoded[source++];
        if (count == 0) {
            uint16_t literal;
            if (source + 2 > encoded_size) return false;
            literal = dsk_read_u16(encoded + source);
            source += 2;
            if (literal > destination_size - output || literal > encoded_size - source) return false;
            memcpy(destination + output, encoded + source, literal);
            source += literal;
            output += literal;
        } else {
            const size_t pattern_size = (size_t)1u << (count & 7u);
            uint16_t repeats;
            if (source + 2 > encoded_size || pattern_size > destination_size - output) return false;
            repeats = dsk_read_u16(encoded + source);
            source += 2;
            if (source + pattern_size > encoded_size ||
                (size_t)repeats * pattern_size > destination_size - output) return false;
            for (uint16_t repeat = 0; repeat < repeats; ++repeat) {
                memcpy(destination + output, encoded + source, pattern_size);
                output += pattern_size;
            }
            source += pattern_size;
        }
    }
    return output == destination_size;
}

static bool dsk_load_td0_data(
    DskImage *image,
    char *error_buffer,
    size_t error_buffer_size)
{
    const uint8_t *data = image->data;
    size_t offset = 12;
    uint8_t max_cylinder = 0;
    uint8_t max_side = 0;
    size_t track_count = 0;
    DskTrack *tracks;

    if (image->data_size < 12 || (data[0] != 'T' && data[0] != 't') || data[1] != 'D') {
        snprintf(error_buffer, error_buffer_size, "File is not a TD0 image.");
        return false;
    }
    if (data[0] == 't') {
        snprintf(error_buffer, error_buffer_size, "Advanced-compressed TD0 images are not supported yet.");
        return false;
    }
    if ((data[7] & 0x80) != 0) {
        if (offset + 4 > image->data_size) return false;
        const uint16_t comment_size = dsk_read_u16(data + offset + 2);
        if ((size_t)comment_size + 4 > image->data_size - offset) return false;
        offset += 4 + comment_size;
    }
    while (offset + 4 <= image->data_size && data[offset] != 0xFF) {
        const uint8_t sectors = data[offset];
        const uint8_t cylinder = data[offset + 1];
        const uint8_t side = data[offset + 2] & 1;
        if (sectors > 64 || offset + 4u + (size_t)sectors * 6u > image->data_size) {
            snprintf(error_buffer, error_buffer_size, "TD0 track is malformed.");
            return false;
        }
        if (cylinder > max_cylinder) max_cylinder = cylinder;
        if (side > max_side) max_side = side;
        ++track_count;
        offset += 4;
        for (uint8_t i = 0; i < sectors; ++i) {
            const uint8_t size_code = data[offset + 3];
            const uint32_t size = dsk_size_from_code(size_code);
            const uint8_t flags = data[offset + 4];
            if (size == 0) {
                snprintf(error_buffer, error_buffer_size, "TD0 sector has an invalid size code.");
                return false;
            }
            offset += 6;
            if ((flags & 0x12) == 0) {
                if (offset + 3 > image->data_size) return false;
                const uint16_t encoded_size = dsk_read_u16(data + offset);
                if (encoded_size < 1 || offset + 3u + encoded_size > image->data_size) return false;
                offset += 3u + encoded_size;
            }
        }
    }
    if (track_count == 0 || max_cylinder >= 80 || max_side > 1) {
        snprintf(error_buffer, error_buffer_size, "TD0 geometry is not supported.");
        return false;
    }
    tracks = (DskTrack *)calloc((size_t)(max_cylinder + 1) * (max_side + 1), sizeof(*tracks));
    if (tracks == NULL) { snprintf(error_buffer, error_buffer_size, "Out of memory while indexing TD0 tracks."); return false; }
    image->tracks = tracks;
    image->owned_sector_data = (uint8_t **)calloc(
        (size_t)(max_cylinder + 1) * (max_side + 1) * 64u,
        sizeof(*image->owned_sector_data));
    if (image->owned_sector_data == NULL) {
        snprintf(error_buffer, error_buffer_size, "Out of memory while decoding TD0 sectors.");
        return false;
    }
    image->cylinder_count = (uint8_t)(max_cylinder + 1);
    image->side_count = (uint8_t)(max_side + 1);
    image->track_slot_count = (size_t)image->cylinder_count * image->side_count;
    offset = 12;
    if ((data[7] & 0x80) != 0) offset += 4 + dsk_read_u16(data + 14);
    while (offset + 4 <= image->data_size && data[offset] != 0xFF) {
        const uint8_t count = data[offset];
        const uint8_t cylinder = data[offset + 1];
        const uint8_t side = data[offset + 2] & 1;
        DskTrack *track = &tracks[(size_t)cylinder * image->side_count + side];
        track->c = cylinder; track->h = side; track->sector_count = count;
        track->sectors = count ? (DskSector *)calloc(count, sizeof(*track->sectors)) : NULL;
        if (count != 0 && track->sectors == NULL) {
            snprintf(error_buffer, error_buffer_size, "Out of memory while indexing TD0 sectors.");
            return false;
        }
        offset += 4;
        for (uint8_t i = 0; i < count; ++i) {
            if (offset + 6 > image->data_size) {
                snprintf(error_buffer, error_buffer_size, "TD0 sector header is truncated.");
                return false;
            }
            const uint8_t *header = data + offset;
            const uint32_t size = dsk_size_from_code(header[3]);
            const uint8_t flags = header[4];
            DskSector *sector = &track->sectors[i];
            sector->c = header[0]; sector->h = header[1]; sector->r = header[2]; sector->n = header[3];
            sector->size = size; sector->stored_size = size;
            offset += 6;
            if ((flags & 0x12) == 0 && size != 0) {
                if (offset + 3 > image->data_size) {
                    snprintf(error_buffer, error_buffer_size, "TD0 sector data is truncated.");
                    return false;
                }
                const uint16_t encoded_size = dsk_read_u16(data + offset);
                const uint8_t encoding = data[offset + 2];
                if (encoded_size < 1 || offset + 3u + encoded_size > image->data_size) {
                    snprintf(error_buffer, error_buffer_size, "TD0 sector data is malformed.");
                    return false;
                }
                uint8_t *decoded = (uint8_t *)calloc(1, size);
                if (decoded == NULL || !dsk_td0_decode_sector(data + offset + 3, encoded_size - 1, encoding, decoded, size)) {
                    free(decoded); snprintf(error_buffer, error_buffer_size, "TD0 sector data is malformed."); return false;
                }
                sector->data = decoded;
                image->owned_sector_data[image->owned_sector_count++] = decoded;
                offset += 3 + encoded_size;
            }
        }
    }
    image->extended = true;
    image->source_read_only = true;
    image->inserted = true;
    return true;
}

static bool dsk_load_udi_standard(
    DskImage *image,
    char *error_buffer,
    size_t error_buffer_size) {
    const uint8_t cylinders = (uint8_t)(image->data[9] + 1u);
    const uint8_t sides = (uint8_t)(image->data[10] + 1u);
    const uint32_t extended_size = dsk_read_u32(image->data + 12);
    const size_t track_count = (size_t)cylinders * sides;
    const size_t normalized_size = track_count * MGT_TRACK_SIZE;
    size_t offset = 16u + extended_size;
    uint8_t *normalized;

    if (cylinders != 80 || sides != 2 || offset > image->data_size) {
        snprintf(error_buffer, error_buffer_size, "UDI geometry is not a standard 80-track MGT disk.");
        return false;
    }
    normalized = (uint8_t *)calloc(1, normalized_size);
    if (normalized == NULL) {
        snprintf(error_buffer, error_buffer_size, "Out of memory while decoding UDI tracks.");
        return false;
    }
    for (size_t track_index = 0; track_index < track_count; ++track_index) {
        const uint8_t *track;
        uint16_t track_data_size;
        uint16_t raw_size;
        const uint8_t *raw;
        size_t cursor;
        size_t track_end;
        if (offset + 5 > image->data_size) {
            free(normalized);
            snprintf(error_buffer, error_buffer_size, "UDI track data is truncated.");
            return false;
        }
        track = image->data + offset;
        if (track[0] != 0x00 && track[0] != 0x01) {
            free(normalized);
            snprintf(error_buffer, error_buffer_size, "UDI contains compressed or nonstandard tracks.");
            return false;
        }
        track_data_size = (uint16_t)(track[1] | ((uint16_t)track[2] << 8));
        if ((size_t)track_data_size + 3u > image->data_size - offset) {
            free(normalized);
            snprintf(error_buffer, error_buffer_size, "UDI track length is invalid.");
            return false;
        }
        raw_size = (uint16_t)(track[3] | ((uint16_t)track[4] << 8));
        raw = track + 5;
        track_end = 5u + raw_size;
        if (track_end > (size_t)track_data_size + 3u ||
            track_end > image->data_size - offset) {
            free(normalized);
            snprintf(error_buffer, error_buffer_size, "UDI raw track data is invalid.");
            return false;
        }
        cursor = 0;
        while (cursor + 8u < raw_size) {
            if (raw[cursor] == 0xFE && cursor + 5u < raw_size) {
                const uint8_t sector = raw[cursor + 3];
                const uint8_t size_code = raw[cursor + 4];
                const uint32_t sector_size = dsk_size_from_code(size_code);
                size_t data_mark = cursor + 5u;
                while (data_mark < raw_size && raw[data_mark] != 0xFB && raw[data_mark] != 0xF8) {
                    ++data_mark;
                }
                if (sector >= 1 && sector <= 10 && sector_size == 512 &&
                    data_mark + 1u + sector_size <= raw_size) {
                    const size_t destination = (track_index * MGT_TRACK_SIZE) +
                        ((size_t)(sector - 1u) * 512u);
                    memcpy(normalized + destination, raw + data_mark + 1u, sector_size);
                }
            }
            ++cursor;
        }
        offset += 3u + track_data_size;
    }
    if (!dsk_load_data_ordered(image, normalized, normalized_size,
                               error_buffer, error_buffer_size, false)) {
        free(normalized);
        return false;
    }
    free(normalized);
    image->source_read_only = true;
    return true;
}

static bool dsk_load_scl_data(
    DskImage *image,
    const uint8_t *data,
    size_t data_size,
    char *error_buffer,
    size_t error_buffer_size) {
    uint8_t *trd;
    size_t directory_offset = 9;
    size_t data_offset;
    size_t next_sector = 16;
    uint8_t file_count;

    if (data_size < 9 || memcmp(data, "SINCLAIR", 8) != 0) {
        snprintf(error_buffer, error_buffer_size, "File is not an SCL image.");
        return false;
    }
    file_count = data[8];
    if (directory_offset + ((size_t)file_count * 14u) > data_size) {
        snprintf(error_buffer, error_buffer_size, "SCL directory is truncated.");
        return false;
    }
    data_offset = directory_offset + ((size_t)file_count * 14u);
    trd = (uint8_t *)calloc(1, TRD_IMAGE_SIZE);
    if (trd == NULL) {
        snprintf(error_buffer, error_buffer_size, "Out of memory while expanding SCL image.");
        return false;
    }
    for (uint8_t file_index = 0; file_index < file_count; ++file_index) {
        const uint8_t *source = data + directory_offset + ((size_t)file_index * 14u);
        uint8_t *entry = trd + ((size_t)file_index * 16u);
        const size_t file_bytes = (size_t)source[13] * 256u;
        const size_t disk_offset = next_sector * 256u;

        if (data_offset > data_size || file_bytes > data_size - data_offset ||
            disk_offset > TRD_IMAGE_SIZE || file_bytes > TRD_IMAGE_SIZE - disk_offset) {
            free(trd);
            snprintf(error_buffer, error_buffer_size, "SCL file data is truncated or too large.");
            return false;
        }
        memcpy(entry, source, 14);
        entry[14] = (uint8_t)(next_sector % 16u + 1u);
        entry[15] = (uint8_t)(next_sector / 16u);
        memcpy(trd + disk_offset, data + data_offset, file_bytes);
        next_sector += source[13];
        data_offset += file_bytes;
    }
    if (!dsk_load_data_ordered(image, trd, TRD_IMAGE_SIZE, error_buffer, error_buffer_size, false)) {
        free(trd);
        return false;
    }
    free(trd);
    image->source_read_only = true;
    return true;
}

static bool dsk_load_sad_data(
    DskImage *image,
    const uint8_t *data,
    size_t data_size,
    char *error_buffer,
    size_t error_buffer_size) {
    const size_t header_size = 22;
    const uint8_t sides = data_size >= header_size ? data[18] : 0;
    const uint8_t cylinders = data_size >= header_size ? data[19] : 0;
    const uint8_t sectors = data_size >= header_size ? data[20] : 0;
    const uint16_t sector_size = data_size >= header_size ? (uint16_t)data[21] * 64u : 0;
    const size_t track_size = (size_t)sectors * sector_size;
    const size_t image_size = (size_t)sides * cylinders * track_size;
    uint8_t *raw;

    if (data_size < header_size || memcmp(data, "Aley's disk backup", 18) != 0 ||
        (sides != 1 && sides != 2) || cylinders == 0 || sectors == 0 ||
        sector_size == 0 || image_size != data_size - header_size) {
        snprintf(error_buffer, error_buffer_size, "Unsupported or invalid SAD image geometry.");
        return false;
    }
    raw = (uint8_t *)malloc(image_size);
    if (raw == NULL) {
        snprintf(error_buffer, error_buffer_size, "Out of memory while expanding SAD image.");
        return false;
    }
    for (uint8_t side = 0; side < sides; ++side) {
        for (uint8_t track = 0; track < cylinders; ++track) {
            const size_t source_offset = header_size +
                (((size_t)side * cylinders + track) * track_size);
            const size_t destination_offset =
                (((size_t)track * sides + side) * track_size);
            memcpy(raw + destination_offset, data + source_offset, track_size);
        }
    }
    if (!dsk_load_data_ordered(image, raw, image_size, error_buffer, error_buffer_size, false)) {
        free(raw);
        return false;
    }
    free(raw);
    return true;
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
            if (image->tracks[index].sectors != NULL)
                free(image->tracks[index].sectors);
        }
    }
    if (image->owned_sector_data != NULL) {
        for (size_t index = 0; index < image->owned_sector_count; ++index) {
            free(image->owned_sector_data[index]);
        }
    }
    free(image->owned_sector_data);
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

/*
    c and h select the physical track under the head.  The controller has
    already matched the command's full C/H/R/N against the ID fields on that
    track, so only R and N are needed to pick the sector here; the ID C/H of
    protected sectors may legitimately differ from the physical position.
*/
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
        if (sector->r == r && (sector->n == n || n == 0xFF)) {
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

    if (image->data_size == MGT_40S_SIZE ||
        image->data_size == MGT_40D_SIZE ||
        image->data_size == MGT_80D_SIZE) {
        return dsk_parse_mgt_data(
            image, error_buffer, error_buffer_size);
    }
    if (image->data_size == TRD_IMAGE_SIZE && !image->raw_side_contiguous) {
        return dsk_parse_trd_data(image, error_buffer, error_buffer_size);
    }
    if (image->data_size == OPD_40S_SIZE || image->data_size == OPD_40D_SIZE) {
        return dsk_parse_opd_data(image, error_buffer, error_buffer_size);
    }
    if (image->data_size >= SDF_HEADER_SIZE && memcmp(image->data, "SDF1", 4) == 0) {
        return dsk_parse_sdf_data(image, error_buffer, error_buffer_size);
    }
    if (image->data_size >= 14 && memcmp(image->data, "FDI", 3) == 0) {
        return dsk_parse_fdi_data(image, error_buffer, error_buffer_size);
    }
    if (image->data_size >= 16 && memcmp(image->data, "UDI!", 4) == 0) {
        return dsk_load_udi_standard(image, error_buffer, error_buffer_size);
    }
    if (image->data_size >= 12 && image->data[1] == 'D' &&
        (image->data[0] == 'T' || image->data[0] == 't')) {
        return dsk_load_td0_data(image, error_buffer, error_buffer_size);
    }
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
            /* Standard DSK stores every sector at the Track-Info size. */
            uint32_t stored_size = image->extended
                ? dsk_read_u16(descriptor + 6)
                : dsk_size_from_code(image->data[file_offset + 0x14]);

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
            sector->descriptor = image->data + file_offset + 0x18 + ((size_t)sector_index * 8u);
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

static bool dsk_load_data_ordered(
    DskImage *image,
    const uint8_t *data,
    size_t data_size,
    char *error_buffer,
    size_t error_buffer_size,
    bool raw_side_contiguous)
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
    loaded.raw_side_contiguous = raw_side_contiguous;

    if (!dsk_parse_owned_data(&loaded, error_buffer, error_buffer_size)) {
        dsk_discard(&loaded);
        return false;
    }

    dsk_discard(image);
    *image = loaded;
    return true;
}

bool dsk_load_data(
    DskImage *image,
    const uint8_t *data,
    size_t data_size,
    char *error_buffer,
    size_t error_buffer_size)
{
    return dsk_load_data_ordered(
        image, data, data_size, error_buffer, error_buffer_size, false);
}

bool dsk_load_file(
    DskImage *image,
    const char *path,
    char *error_buffer,
    size_t error_buffer_size)
{
    FILE *file;
    uint8_t *data;
    int64_t file_size;
    bool loaded;
    const char *extension;

    if (path == NULL || path[0] == '\0') {
        snprintf(error_buffer, error_buffer_size, "No DSK path was provided.");
        return false;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        snprintf(error_buffer, error_buffer_size, "Could not open DSK image: %s", path);
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (file_size = _ftelli64(file)) <= 0) {
        fclose(file);
        snprintf(error_buffer, error_buffer_size, "Could not determine DSK image size: %s", path);
        return false;
    }
    if (file_size > (int64_t)DSK_MAX_IMAGE_SIZE) {
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

    extension = strrchr(path, '.');
    if (extension != NULL && _stricmp(extension, ".scl") == 0) {
        loaded = dsk_load_scl_data(image, data, (size_t)file_size, error_buffer, error_buffer_size);
        if (loaded) {
            snprintf(image->path, sizeof(image->path), "%s", path);
        }
        free(data);
        return loaded;
    }
    if (extension != NULL && _stricmp(extension, ".sad") == 0) {
        loaded = dsk_load_sad_data(image, data, (size_t)file_size, error_buffer, error_buffer_size);
        if (loaded) {
            snprintf(image->path, sizeof(image->path), "%s", path);
        }
        free(data);
        return loaded;
    }
    {
        const bool side_contiguous = extension != NULL &&
            _stricmp(extension, ".img") == 0;
        loaded = dsk_load_data_ordered(
            image,
            data,
            (size_t)file_size,
            error_buffer,
            error_buffer_size,
            side_contiguous);
    }
    free(data);
    if (loaded) {
        snprintf(image->path, sizeof(image->path), "%s", path);
    }
    return loaded;
}

bool dsk_save_file(
    DskImage *image,
    char *error_buffer,
    size_t error_buffer_size)
{
    FileWriteChunk chunk;

    if (image != NULL && image->source_read_only) {
        snprintf(error_buffer, error_buffer_size, "This disk image is read-only.");
        return false;
    }
    if (image == NULL || !image->inserted || image->data == NULL ||
        image->data_size == 0 || image->path[0] == '\0') {
        snprintf(error_buffer, error_buffer_size, "No disk is inserted.");
        return false;
    }

    chunk.data = image->data;
    chunk.size = image->data_size;
    if (!file_write_atomic(
            image->path,
            &chunk,
            1,
            "disk image",
            error_buffer,
            error_buffer_size)) {
        return false;
    }
    image->dirty = false;
    return true;
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
    const bool deleted = (drive & DSK_WRITE_DELETED) != 0;

    drive &= (uint8_t)~DSK_WRITE_DELETED;
    if (buffer == NULL && data_size == 0) {
        /* Write-protect query used by the controller. */
        return dsk_drive_ready(image, drive) && !image->source_read_only;
    }
    if (!dsk_drive_ready(image, drive) || buffer == NULL || image->source_read_only) {
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
    /* Persist the fresh status so a saved image does not keep old marks. */
    sector->st1 = 0;
    sector->st2 = deleted ? 0x40 : 0;
    if (sector->descriptor != NULL) {
        sector->descriptor[4] = sector->st1;
        sector->descriptor[5] = sector->st2;
    }
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
    /*
        The controller uses a failed lookup to detect the index hole and wrap
        to the first sector.  Returning a modulo-indexed sector here hid the
        physical end of a track and made Read ID drift independently forever.
    */
    if (index >= track->sector_count) {
        return false;
    }
    sector = &track->sectors[index];
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
