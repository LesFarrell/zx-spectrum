#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "file_util.h"

#include <io.h>
#include <stdio.h>
#include <string.h>

static void file_remove_temporary(const char *path) {
    if (path != NULL && path[0] != '\0') {
        DeleteFileA(path);
    }
}

static bool file_make_temporary_path(
    const char *path,
    char temporary_path[MAX_PATH],
    const char *description,
    char *error_buffer,
    size_t error_buffer_size)
{
    char directory[MAX_PATH];
    const char *last_forward_slash = strrchr(path, '/');
    const char *last_backslash = strrchr(path, '\\');
    const char *last_separator = last_forward_slash;

    if (last_separator == NULL ||
        (last_backslash != NULL && last_backslash > last_separator)) {
        last_separator = last_backslash;
    }
    if (last_separator == NULL) {
        snprintf(directory, sizeof(directory), ".");
    } else {
        const size_t directory_length = (size_t)(last_separator - path) + 1u;
        if (directory_length >= sizeof(directory)) {
            snprintf(error_buffer, error_buffer_size, "The %s path is too long.", description);
            return false;
        }
        memcpy(directory, path, directory_length);
        directory[directory_length] = '\0';
    }
    if (GetTempFileNameA(directory, "zxs", 0, temporary_path) == 0) {
        snprintf(error_buffer, error_buffer_size, "Could not create a temporary %s.", description);
        return false;
    }
    return true;
}

bool file_write_atomic(
    const char *path,
    const FileWriteChunk *chunks,
    size_t chunk_count,
    const char *description,
    char *error_buffer,
    size_t error_buffer_size)
{
    char temporary_path[MAX_PATH] = {0};
    FILE *file;
    bool ok = true;

    if (path == NULL || path[0] == '\0' || chunks == NULL || chunk_count == 0) {
        snprintf(error_buffer, error_buffer_size, "No %s destination was supplied.", description);
        return false;
    }
    if (!file_make_temporary_path(
            path,
            temporary_path,
            description,
            error_buffer,
            error_buffer_size)) {
        return false;
    }

    file = fopen(temporary_path, "wb");
    if (file == NULL) {
        file_remove_temporary(temporary_path);
        snprintf(error_buffer, error_buffer_size, "Could not open the temporary %s.", description);
        return false;
    }
    for (size_t i = 0; i < chunk_count; ++i) {
        if (chunks[i].size == 0) {
            continue;
        }
        if (chunks[i].data == NULL ||
            fwrite(chunks[i].data, 1, chunks[i].size, file) != chunks[i].size) {
            ok = false;
            break;
        }
    }
    if (ok && (fflush(file) != 0 || _commit(_fileno(file)) != 0)) {
        ok = false;
    }
    if (fclose(file) != 0) {
        ok = false;
    }
    if (!ok) {
        file_remove_temporary(temporary_path);
        snprintf(error_buffer, error_buffer_size, "Could not save the complete %s.", description);
        return false;
    }
    if (!MoveFileExA(
            temporary_path,
            path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        file_remove_temporary(temporary_path);
        snprintf(error_buffer, error_buffer_size, "Could not replace the destination %s.", description);
        return false;
    }
    return true;
}
