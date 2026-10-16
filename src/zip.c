#include "zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "szx_inflate.h"

enum {
    ZIP_EOCD_MIN_SIZE = 22,
    ZIP_EOCD_SEARCH_SIZE = 65535 + ZIP_EOCD_MIN_SIZE,
    ZIP_CENTRAL_HEADER_SIZE = 46,
    ZIP_LOCAL_HEADER_SIZE = 30,
    ZIP_MAX_ENTRIES = 16384,
    ZIP_MAX_ARCHIVE_SIZE = 128 * 1024 * 1024
};

static uint16_t zip_u16(const uint8_t *data) {
    return (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
}

static uint32_t zip_u32(const uint8_t *data) {
    return (uint32_t)data[0] |
        ((uint32_t)data[1] << 8) |
        ((uint32_t)data[2] << 16) |
        ((uint32_t)data[3] << 24);
}

static uint32_t zip_crc32(const uint8_t *data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    while (size-- != 0) {
        crc ^= *data++;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return ~crc;
}

void zip_init(ZipArchive *archive) {
    if (archive != NULL) {
        memset(archive, 0, sizeof(*archive));
    }
}

void zip_discard(ZipArchive *archive) {
    if (archive == NULL) {
        return;
    }
    if (archive->entries != NULL) {
        for (size_t index = 0; index < archive->entry_count; ++index) {
            free(archive->entries[index].name);
        }
    }
    free(archive->entries);
    free(archive->data);
    zip_init(archive);
}

static bool zip_read_file(
    const char *path,
    uint8_t **out_data,
    size_t *out_size,
    char *error_buffer,
    size_t error_buffer_size)
{
    FILE *file = fopen(path, "rb");
    int64_t file_size;
    uint8_t *data;

    if (file == NULL) {
        snprintf(error_buffer, error_buffer_size, "Could not open ZIP archive: %s", path);
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (file_size = _ftelli64(file)) < ZIP_EOCD_MIN_SIZE) {
        fclose(file);
        snprintf(error_buffer, error_buffer_size, "The ZIP archive is empty or truncated: %s", path);
        return false;
    }
    if ((uint64_t)file_size > SIZE_MAX ||
        (uint64_t)file_size > ZIP_MAX_ARCHIVE_SIZE) {
        fclose(file);
        snprintf(error_buffer, error_buffer_size, "The ZIP archive exceeds the 128 MB safety limit.");
        return false;
    }
    rewind(file);
    data = (uint8_t *)malloc((size_t)file_size);
    if (data == NULL) {
        fclose(file);
        snprintf(error_buffer, error_buffer_size, "Out of memory while reading ZIP archive.");
        return false;
    }
    if (fread(data, 1, (size_t)file_size, file) != (size_t)file_size) {
        free(data);
        fclose(file);
        snprintf(error_buffer, error_buffer_size, "Could not read the complete ZIP archive: %s", path);
        return false;
    }
    fclose(file);
    *out_data = data;
    *out_size = (size_t)file_size;
    return true;
}

bool zip_open_file(
    ZipArchive *archive,
    const char *path,
    char *error_buffer,
    size_t error_buffer_size)
{
    ZipArchive loaded;
    size_t search_start;
    size_t eocd = SIZE_MAX;
    size_t central_offset;
    size_t central_size;
    size_t central_end;
    size_t position;
    uint16_t entry_count;

    if (archive == NULL || path == NULL) {
        snprintf(error_buffer, error_buffer_size, "Invalid ZIP open request.");
        return false;
    }
    zip_init(&loaded);
    if (!zip_read_file(path, &loaded.data, &loaded.size, error_buffer, error_buffer_size)) {
        return false;
    }

    search_start = loaded.size > ZIP_EOCD_SEARCH_SIZE
        ? loaded.size - ZIP_EOCD_SEARCH_SIZE
        : 0;
    for (size_t offset = loaded.size - ZIP_EOCD_MIN_SIZE + 1; offset-- > search_start;) {
        if (zip_u32(loaded.data + offset) == 0x06054B50u) {
            const uint16_t comment_length = zip_u16(loaded.data + offset + 20);
            if (offset + ZIP_EOCD_MIN_SIZE + comment_length == loaded.size) {
                eocd = offset;
                break;
            }
        }
    }
    if (eocd == SIZE_MAX) {
        zip_discard(&loaded);
        snprintf(error_buffer, error_buffer_size, "The file is not a valid ZIP archive.");
        return false;
    }
    if (zip_u16(loaded.data + eocd + 4) != 0 ||
        zip_u16(loaded.data + eocd + 6) != 0 ||
        zip_u16(loaded.data + eocd + 8) != zip_u16(loaded.data + eocd + 10)) {
        zip_discard(&loaded);
        snprintf(error_buffer, error_buffer_size, "Multi-disk ZIP archives are not supported.");
        return false;
    }
    entry_count = zip_u16(loaded.data + eocd + 10);
    central_size = zip_u32(loaded.data + eocd + 12);
    central_offset = zip_u32(loaded.data + eocd + 16);
    if (entry_count == 0xFFFFu || central_size == 0xFFFFFFFFu ||
        central_offset == 0xFFFFFFFFu) {
        zip_discard(&loaded);
        snprintf(error_buffer, error_buffer_size, "ZIP64 archives are not supported.");
        return false;
    }
    if (entry_count > ZIP_MAX_ENTRIES || central_offset > loaded.size ||
        central_size > loaded.size - central_offset ||
        central_offset + central_size > eocd) {
        zip_discard(&loaded);
        snprintf(error_buffer, error_buffer_size, "The ZIP central directory is invalid.");
        return false;
    }
    central_end = central_offset + central_size;

    loaded.entries = (ZipEntry *)calloc(entry_count, sizeof(*loaded.entries));
    if (entry_count != 0 && loaded.entries == NULL) {
        zip_discard(&loaded);
        snprintf(error_buffer, error_buffer_size, "Out of memory while indexing ZIP archive.");
        return false;
    }
    loaded.entry_count = entry_count;
    position = central_offset;
    for (size_t index = 0; index < entry_count; ++index) {
        ZipEntry *entry = &loaded.entries[index];
        uint16_t name_length;
        uint16_t extra_length;
        uint16_t comment_length;
        size_t record_size;

        if (position > central_end || ZIP_CENTRAL_HEADER_SIZE > central_end - position ||
            zip_u32(loaded.data + position) != 0x02014B50u) {
            zip_discard(&loaded);
            snprintf(error_buffer, error_buffer_size, "The ZIP central directory is truncated.");
            return false;
        }
        name_length = zip_u16(loaded.data + position + 28);
        extra_length = zip_u16(loaded.data + position + 30);
        comment_length = zip_u16(loaded.data + position + 32);
        record_size = ZIP_CENTRAL_HEADER_SIZE + (size_t)name_length +
            extra_length + comment_length;
        if (record_size > central_end - position || name_length == 0) {
            zip_discard(&loaded);
            snprintf(error_buffer, error_buffer_size, "A ZIP directory entry is invalid.");
            return false;
        }
        entry->flags = zip_u16(loaded.data + position + 8);
        entry->method = zip_u16(loaded.data + position + 10);
        entry->crc32 = zip_u32(loaded.data + position + 16);
        entry->compressed_size = zip_u32(loaded.data + position + 20);
        entry->uncompressed_size = zip_u32(loaded.data + position + 24);
        entry->local_header_offset = zip_u32(loaded.data + position + 42);
        if (zip_u16(loaded.data + position + 34) != 0) {
            zip_discard(&loaded);
            snprintf(error_buffer, error_buffer_size, "A ZIP entry starts on another disk.");
            return false;
        }
        if (memchr(
                loaded.data + position + ZIP_CENTRAL_HEADER_SIZE,
                '\0',
                name_length) != NULL) {
            zip_discard(&loaded);
            snprintf(error_buffer, error_buffer_size, "A ZIP entry name contains a null byte.");
            return false;
        }
        entry->name = (char *)malloc((size_t)name_length + 1);
        if (entry->name == NULL) {
            zip_discard(&loaded);
            snprintf(error_buffer, error_buffer_size, "Out of memory while reading ZIP entry names.");
            return false;
        }
        entry->name_length = name_length;
        memcpy(
            entry->name,
            loaded.data + position + ZIP_CENTRAL_HEADER_SIZE,
            name_length);
        entry->name[name_length] = '\0';
        entry->is_directory = name_length != 0 &&
            (loaded.data[position + ZIP_CENTRAL_HEADER_SIZE + name_length - 1] == '/' ||
             loaded.data[position + ZIP_CENTRAL_HEADER_SIZE + name_length - 1] == '\\');
        if (entry->compressed_size == 0xFFFFFFFFu ||
            entry->uncompressed_size == 0xFFFFFFFFu ||
            entry->local_header_offset == 0xFFFFFFFFu) {
            zip_discard(&loaded);
            snprintf(error_buffer, error_buffer_size, "ZIP64 entries are not supported.");
            return false;
        }
        position += record_size;
    }

    zip_discard(archive);
    *archive = loaded;
    return true;
}

bool zip_extract_entry(
    const ZipArchive *archive,
    size_t entry_index,
    uint8_t **data,
    size_t *size,
    char *error_buffer,
    size_t error_buffer_size)
{
    const ZipEntry *entry;
    size_t offset;
    size_t compressed_offset;
    uint16_t name_length;
    uint16_t extra_length;
    uint8_t *output;
    bool expanded;

    if (archive == NULL || data == NULL || size == NULL || entry_index >= archive->entry_count) {
        snprintf(error_buffer, error_buffer_size, "Invalid ZIP extraction request.");
        return false;
    }
    *data = NULL;
    *size = 0;
    entry = &archive->entries[entry_index];
    if (entry->is_directory) {
        snprintf(error_buffer, error_buffer_size, "The selected ZIP entry is a directory.");
        return false;
    }
    if ((entry->flags & 1u) != 0) {
        snprintf(error_buffer, error_buffer_size, "Encrypted ZIP entries are not supported: %s", entry->name);
        return false;
    }
    if ((entry->flags & (uint16_t)~0x080Eu) != 0) {
        snprintf(error_buffer, error_buffer_size, "Unsupported ZIP entry flags for %s.", entry->name);
        return false;
    }
    if (entry->method != 0 && entry->method != 8) {
        snprintf(
            error_buffer,
            error_buffer_size,
            "ZIP compression method %u is not supported for %s.",
            entry->method,
            entry->name);
        return false;
    }
    if (entry->uncompressed_size == 0 || entry->uncompressed_size > ZIP_MAX_EXTRACTED_SIZE) {
        snprintf(error_buffer, error_buffer_size, "ZIP entry is empty or exceeds the 128 MB safety limit: %s", entry->name);
        return false;
    }
    offset = entry->local_header_offset;
    if (offset > archive->size || ZIP_LOCAL_HEADER_SIZE > archive->size - offset ||
        zip_u32(archive->data + offset) != 0x04034B50u) {
        snprintf(error_buffer, error_buffer_size, "The ZIP local header is invalid: %s", entry->name);
        return false;
    }
    if (zip_u16(archive->data + offset + 6) != entry->flags ||
        zip_u16(archive->data + offset + 8) != entry->method) {
        snprintf(error_buffer, error_buffer_size, "ZIP entry headers disagree: %s", entry->name);
        return false;
    }
    name_length = zip_u16(archive->data + offset + 26);
    extra_length = zip_u16(archive->data + offset + 28);
    if (name_length != entry->name_length ||
        ZIP_LOCAL_HEADER_SIZE + (size_t)name_length > archive->size - offset ||
        memcmp(
            archive->data + offset + ZIP_LOCAL_HEADER_SIZE,
            entry->name,
            name_length) != 0) {
        snprintf(error_buffer, error_buffer_size, "ZIP entry names disagree between headers: %s", entry->name);
        return false;
    }
    compressed_offset = offset + ZIP_LOCAL_HEADER_SIZE + (size_t)name_length + extra_length;
    if (compressed_offset > archive->size ||
        entry->compressed_size > archive->size - compressed_offset) {
        snprintf(error_buffer, error_buffer_size, "The ZIP entry data is truncated: %s", entry->name);
        return false;
    }

    output = (uint8_t *)malloc(entry->uncompressed_size);
    if (output == NULL) {
        snprintf(error_buffer, error_buffer_size, "Out of memory while extracting %s.", entry->name);
        return false;
    }
    if (entry->method == 0) {
        expanded = entry->compressed_size == entry->uncompressed_size;
        if (expanded) {
            memcpy(output, archive->data + compressed_offset, entry->uncompressed_size);
        }
    } else {
        expanded = szx_inflate_raw(
            archive->data + compressed_offset,
            entry->compressed_size,
            output,
            entry->uncompressed_size);
    }
    if (!expanded || zip_crc32(output, entry->uncompressed_size) != entry->crc32) {
        free(output);
        snprintf(error_buffer, error_buffer_size, "Could not decompress or verify ZIP entry: %s", entry->name);
        return false;
    }
    *data = output;
    *size = entry->uncompressed_size;
    return true;
}
