#ifndef NEXT_SD_H
#define NEXT_SD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum {
    NEXT_SD_BLOCK_SIZE = 512,
    NEXT_SD_RESPONSE_CAPACITY = 1024
};

typedef struct NextSdCard {
    FILE *image;
    uint64_t image_size;
    bool writable;
    bool selected;
    bool idle;
    bool app_command;
    bool high_capacity;

    uint8_t command[6];
    unsigned command_length;
    uint64_t command_count;
    uint8_t last_command;
    uint32_t last_argument;
    uint8_t response[NEXT_SD_RESPONSE_CAPACITY];
    size_t response_read;
    size_t response_write;

    bool multi_read;
    uint32_t multi_read_lba;
    bool write_pending;
    bool write_multi;
    uint32_t write_lba;
    uint8_t write_buffer[NEXT_SD_BLOCK_SIZE + 3];
    size_t write_length;
} NextSdCard;

void next_sd_init(NextSdCard *card);
void next_sd_close(NextSdCard *card);
bool next_sd_open_image(
    NextSdCard *card,
    const char *path,
    bool writable,
    char *error_buffer,
    size_t error_buffer_size
);
void next_sd_set_high_capacity(NextSdCard *card, bool enabled);
void next_sd_write_chip_select(NextSdCard *card, uint8_t value);
void next_sd_write_data(NextSdCard *card, uint8_t value);
uint8_t next_sd_read_data(NextSdCard *card);

#endif
