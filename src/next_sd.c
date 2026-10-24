#include "next_sd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void next_sd_set_error(char *buffer, size_t size, const char *message)
{
    if (buffer != NULL && size > 0)
        snprintf(buffer, size, "%s", message);
}

static bool next_sd_seek(FILE *file, uint64_t offset)
{
#ifdef _WIN32
    return _fseeki64(file, (__int64)offset, SEEK_SET) == 0;
#else
    return fseek(file, (long)offset, SEEK_SET) == 0;
#endif
}

static void next_sd_clear_response(NextSdCard *card)
{
    card->response_read = 0;
    card->response_write = 0;
}

static void next_sd_queue_byte(NextSdCard *card, uint8_t value)
{
    if (card->response_read == card->response_write && card->response_write != 0)
    {
        next_sd_clear_response(card);
    }
    if (card->response_write < NEXT_SD_RESPONSE_CAPACITY)
    {
        card->response[card->response_write++] = value;
    }
}

static void next_sd_queue_r1(NextSdCard *card, uint8_t value)
{
    next_sd_queue_byte(card, 0xFF);
    next_sd_queue_byte(card, 0xFF);
    next_sd_queue_byte(card, value);
}

static uint16_t next_sd_crc16(const uint8_t *data, size_t size)
{
    uint16_t crc = 0;
    while (size-- > 0)
    {
        crc ^= (uint16_t)*data++ << 8;
        for (unsigned bit = 0; bit < 8; ++bit)
        {
            crc = (crc & 0x8000) != 0 ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static bool next_sd_read_block(NextSdCard *card, uint32_t lba, uint8_t *data)
{
    const uint64_t offset = (uint64_t)lba * NEXT_SD_BLOCK_SIZE;
    if (card->image == NULL || offset > card->image_size ||
        NEXT_SD_BLOCK_SIZE > card->image_size - offset)
        return false;
    if (!next_sd_seek(card->image, offset) ||
        fread(data, 1, NEXT_SD_BLOCK_SIZE, card->image) != NEXT_SD_BLOCK_SIZE)
    {
        return false;
    }
    return true;
}

static bool next_sd_write_block(NextSdCard *card, uint32_t lba, const uint8_t *data)
{
    const uint64_t offset = (uint64_t)lba * NEXT_SD_BLOCK_SIZE;
    if (card->image == NULL || !card->writable || offset > card->image_size ||
        NEXT_SD_BLOCK_SIZE > card->image_size - offset)
        return false;
    if (!next_sd_seek(card->image, offset))
        return false;
    if (fwrite(data, 1, NEXT_SD_BLOCK_SIZE, card->image) != NEXT_SD_BLOCK_SIZE)
        return false;
    return fflush(card->image) == 0;
}

static uint32_t next_sd_command_lba(const NextSdCard *card, uint32_t argument)
{
    return card->high_capacity ? argument : argument / NEXT_SD_BLOCK_SIZE;
}

static bool next_sd_block_in_range(const NextSdCard *card, uint32_t lba)
{
    return (uint64_t)lba < card->image_size / NEXT_SD_BLOCK_SIZE;
}

static void next_sd_queue_data(NextSdCard *card, const uint8_t *data, size_t size)
{
    const uint16_t crc = next_sd_crc16(data, size);
    /* The Next firmware expects the card's access-time byte between the
       command response and the data token.  It also consumes this gap when
       continuing a CMD18 stream across block boundaries. */
    next_sd_queue_byte(card, 0xFF);
    next_sd_queue_byte(card, 0xFE);
    for (size_t i = 0; i < size; ++i)
        next_sd_queue_byte(card, data[i]);
    next_sd_queue_byte(card, (uint8_t)(crc >> 8));
    next_sd_queue_byte(card, (uint8_t)crc);
}

static bool next_sd_queue_block(NextSdCard *card, uint32_t lba, bool include_r1)
{
    uint8_t data[NEXT_SD_BLOCK_SIZE];
    if (!next_sd_read_block(card, lba, data))
    {
        if (include_r1)
        {
            next_sd_queue_r1(card, 0x20);
        }
        else
        {
            /* A CMD18 continuation cannot fail at the R1 stage, so report
               it with a data error token (out of range or generic error). */
            next_sd_queue_byte(card, 0xFF);
            next_sd_queue_byte(card, next_sd_block_in_range(card, lba) ? 0x01 : 0x08);
            card->multi_read = false;
        }
        return false;
    }
    if (include_r1)
        next_sd_queue_r1(card, 0x00);
    next_sd_queue_data(card, data, sizeof(data));
    return true;
}

static void next_sd_build_csd(const NextSdCard *card, uint8_t csd[16])
{
    const uint64_t sdsc_limit = UINT64_C(1) * 1024 * 1024 * 1024;
    const uint64_t capacity = card->image_size < sdsc_limit
                                  ? card->image_size
                                  : sdsc_limit;
    const uint64_t capacity_units = capacity / (512 * 512);
    const uint64_t c_size = capacity_units != 0 ? capacity_units - 1 : 0;
    const uint64_t device_size = c_size << 6;

    if (card->high_capacity)
    {
        /* OCR CCS is set, so report a v2 CSD whose C_SIZE counts 512 KB
           units and matches block addressing of the whole image. */
        const uint64_t units = card->image_size / (512 * 1024);
        const uint64_t v2_size = units != 0 ? units - 1 : 0;
        memset(csd, 0, 16);
        csd[0] = 0x40;
        csd[1] = 0x0E;
        csd[3] = 0x32;
        csd[4] = 0x5B;
        csd[5] = 0x59;
        csd[7] = (uint8_t)((v2_size >> 16) & 0x3F);
        csd[8] = (uint8_t)(v2_size >> 8);
        csd[9] = (uint8_t)v2_size;
        csd[10] = 0x7F;
        csd[11] = 0x80;
        csd[12] = 0x0A;
        csd[13] = 0x40;
        csd[15] = 0x01;
        return;
    }

    /* Match the v1/SDSC geometry returned by the Next reference model.
       The legacy NextZXOS driver derives byte addressing and media size
       from these fields before it mounts the FAT volume. */
    memset(csd, 0x0B, 16);
    csd[5] = 9;
    csd[6] = (uint8_t)((device_size >> 16) & 3);
    csd[7] = (uint8_t)(device_size >> 8);
    csd[8] = (uint8_t)(device_size & 0xC0);
    csd[9] = 3;
    csd[10] = 0x80;
}

static void next_sd_dispatch_app(NextSdCard *card, uint8_t command)
{
    card->app_command = false;
    switch (command)
    {
    case 41:
        card->idle = false;
        next_sd_queue_r1(card, 0x00);
        break;
    case 13:
        next_sd_queue_r1(card, 0x00);
        next_sd_queue_byte(card, 0x00);
        break;
    case 51:
    {
        static const uint8_t scr[8] = {0x02, 0x35, 0x83, 0, 0, 0, 0, 0};
        next_sd_queue_r1(card, 0x00);
        next_sd_queue_data(card, scr, sizeof(scr));
        break;
    }
    default:
        next_sd_queue_r1(card, 0x04);
        break;
    }
}

static void next_sd_dispatch(NextSdCard *card)
{
    const uint8_t command = card->command[0] & 0x3F;
    const uint32_t argument = ((uint32_t)card->command[1] << 24) |
                              ((uint32_t)card->command[2] << 16) |
                              ((uint32_t)card->command[3] << 8) |
                              card->command[4];
    ++card->command_count;
    if (card->app_command)
    {
        next_sd_dispatch_app(card, command);
        return;
    }
    switch (command)
    {
    case 0:
        card->idle = true;
        card->multi_read = false;
        next_sd_queue_r1(card, 0x01);
        break;
    case 1:
        card->idle = false;
        next_sd_queue_r1(card, 0x00);
        break;
    case 8:
        next_sd_queue_r1(card, card->idle ? 0x01 : 0x00);
        for (unsigned i = 1; i <= 4; ++i)
            next_sd_queue_byte(card, card->command[i]);
        break;
    case 9:
    {
        uint8_t csd[16];
        next_sd_build_csd(card, csd);
        next_sd_queue_r1(card, 0x00);
        next_sd_queue_data(card, csd, sizeof(csd));
        break;
    }
    case 10:
    {
        static const uint8_t cid[16] = {
            0x03, 'N', 'X', 'E', 'M', 'U', '0', '1',
            0, 0, 0, 1, 0x01, 0xA0, 0, 0};
        next_sd_queue_r1(card, 0x00);
        next_sd_queue_data(card, cid, sizeof(cid));
        break;
    }
    case 12:
        card->multi_read = false;
        next_sd_clear_response(card);
        next_sd_queue_r1(card, 0x00);
        break;
    case 13:
        next_sd_queue_r1(card, 0x00);
        next_sd_queue_byte(card, 0x00);
        break;
    case 16:
    case 59:
        next_sd_queue_r1(card, 0x00);
        break;
    case 17:
        next_sd_queue_block(card, next_sd_command_lba(card, argument), true);
        break;
    case 18:
        card->multi_read_lba = next_sd_command_lba(card, argument);
        if (!next_sd_block_in_range(card, card->multi_read_lba))
        {
            next_sd_queue_r1(card, 0x20);
            break;
        }
        card->multi_read = next_sd_queue_block(card, card->multi_read_lba++, true);
        break;
    case 24:
    case 25:
        card->write_lba = next_sd_command_lba(card, argument);
        if (!next_sd_block_in_range(card, card->write_lba))
        {
            next_sd_queue_r1(card, 0x20);
            break;
        }
        card->write_pending = true;
        card->write_multi = command == 25;
        card->write_length = 0;
        next_sd_queue_r1(card, 0x00);
        break;
    case 55:
        card->app_command = true;
        next_sd_queue_r1(card, card->idle ? 0x01 : 0x00);
        break;
    case 58:
    {
        uint8_t ocr = 0x80;
        if (card->high_capacity)
            ocr |= 0x40;
        next_sd_queue_r1(card, card->idle ? 0x01 : 0x00);
        next_sd_queue_byte(card, ocr);
        next_sd_queue_byte(card, 0x00);
        next_sd_queue_byte(card, 0x00);
        next_sd_queue_byte(card, 0x00);
        break;
    }
    default:
        next_sd_queue_r1(card, 0x04 | (card->idle ? 1 : 0));
        break;
    }
}

void next_sd_init(NextSdCard *card)
{
    memset(card, 0, sizeof(*card));
    card->idle = true;
}

void next_sd_close(NextSdCard *card)
{
    if (card == NULL)
        return;
    if (card->image != NULL)
        fclose(card->image);
    next_sd_init(card);
}

bool next_sd_open_image(
    NextSdCard *card,
    const char *path,
    bool writable,
    char *error_buffer,
    size_t error_buffer_size)
{
    FILE *file;
    uint64_t size;
    if (card == NULL || path == NULL)
    {
        next_sd_set_error(error_buffer, error_buffer_size, "Invalid Next SD image path.");
        return false;
    }
    file = fopen(path, writable ? "r+b" : "rb");
    if (file == NULL)
    {
        next_sd_set_error(error_buffer, error_buffer_size, "Unable to open the Next SD image.");
        return false;
    }
#ifdef _WIN32
    if (_fseeki64(file, 0, SEEK_END) != 0 || (size = (uint64_t)_ftelli64(file)) == (uint64_t)-1)
    {
#else
    if (fseek(file, 0, SEEK_END) != 0 || (size = (uint64_t)ftell(file)) == (uint64_t)-1)
    {
#endif
        fclose(file);
        next_sd_set_error(error_buffer, error_buffer_size, "Unable to measure the Next SD image.");
        return false;
    }
    if (size < NEXT_SD_BLOCK_SIZE || size % NEXT_SD_BLOCK_SIZE != 0)
    {
        fclose(file);
        next_sd_set_error(error_buffer, error_buffer_size,
                          "The Next SD image size is not a whole number of 512-byte sectors.");
        return false;
    }
    next_sd_close(card);
    card->image = file;
    card->image_size = size;
    card->writable = writable;
    card->idle = true;
    return true;
}

void next_sd_write_chip_select(NextSdCard *card, uint8_t value)
{
    const bool selected = (value & 0x01) == 0;
    if (card->selected != selected)
    {
        card->command_length = 0;
        next_sd_clear_response(card);
        card->write_pending = false;
        card->write_multi = false;
        card->write_length = 0;
    }
    card->selected = selected;
}

void next_sd_write_data(NextSdCard *card, uint8_t value)
{
    if (card == NULL || !card->selected || card->image == NULL)
        return;
    if (card->write_pending && card->write_length == 0 && card->write_multi)
    {
        if (value == 0xFD)
        {
            /* Stop-tran token: one skipped byte, then a short busy. */
            card->write_pending = false;
            card->write_multi = false;
            next_sd_clear_response(card);
            next_sd_queue_byte(card, 0xFF);
            next_sd_queue_byte(card, 0x00);
            next_sd_queue_byte(card, 0xFF);
            return;
        }
        if ((value & 0xC0) == 0x40)
        {
            /* After a write error the host may abort with CMD12. */
            card->write_pending = false;
            card->write_multi = false;
        }
    }
    if (card->write_pending)
    {
        if (card->write_length == 0 && value != (card->write_multi ? 0xFC : 0xFE))
            return;
        if (card->write_length < sizeof(card->write_buffer))
        {
            card->write_buffer[card->write_length++] = value;
        }
        if (card->write_length == sizeof(card->write_buffer))
        {
            const bool written = next_sd_write_block(card, card->write_lba,
                                                     card->write_buffer + 1);
            card->write_length = 0;
            if (card->write_multi)
            {
                /* CMD25 keeps accepting packets until the stop-tran token. */
                ++card->write_lba;
                next_sd_clear_response(card);
            }
            else
            {
                card->write_pending = false;
            }
            next_sd_queue_byte(card, written ? 0x05 : 0x0D);
            next_sd_queue_byte(card, 0x00);
            next_sd_queue_byte(card, 0xFF);
        }
        return;
    }
    if (card->command_length == 0 && (value & 0xC0) != 0x40)
        return;
    card->command[card->command_length++] = value;
    if (card->command_length == sizeof(card->command))
    {
        card->command_length = 0;
        next_sd_dispatch(card);
    }
}

uint8_t next_sd_read_data(NextSdCard *card)
{
    uint8_t value = 0xFF;
    if (card == NULL || !card->selected || card->image == NULL)
        return 0xFF;
    if (card->response_read == card->response_write)
    {
        next_sd_clear_response(card);
        if (card->multi_read)
        {
            next_sd_queue_byte(card, 0xFF);
            next_sd_queue_byte(card, 0xFF);
            next_sd_queue_block(card, card->multi_read_lba++, false);
        }
    }
    if (card->response_read < card->response_write)
    {
        value = card->response[card->response_read++];
    }
    return value;
}
