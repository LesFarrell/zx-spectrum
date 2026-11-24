#include "disk.h"
#include "microdrive.h"
#include "szx_inflate.h"
#include "tape.h"
#include "wd1772.h"
#include "zip.h"

#define CHIPS_IMPL
#include "chips/z80.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

static void make_tap(uint8_t *tap, size_t *tap_size)
{
    static const char name[10] = {'T','E','S','T',' ',' ',' ',' ',' ',' '};
    uint8_t header[19];
    uint8_t body[4];
    uint8_t header_checksum = 0;
    uint8_t body_checksum = 0;

    memset(header, 0, sizeof(header));
    header[0] = 0;
    header[1] = 0;
    memcpy(header + 2, name, sizeof(name));
    header[12] = 0;
    header[13] = 0x80;
    header[14] = 2;
    header[15] = 0;
    header[16] = 0;
    header[17] = 2;
    for (size_t i = 0; i < sizeof(header) - 1; ++i)
        header_checksum ^= header[i];
    header[18] = header_checksum;

    body[0] = 0xFF;
    body[1] = 0x42;
    body[2] = 0x43;
    body_checksum = body[0] ^ body[1] ^ body[2];
    body[3] = body_checksum;

    tap[0] = (uint8_t)sizeof(header);
    tap[1] = 0;
    memcpy(tap + 2, header, sizeof(header));
    tap[21] = (uint8_t)sizeof(body);
    tap[22] = 0;
    memcpy(tap + 23, body, sizeof(body));
    *tap_size = 27;
}

typedef struct FakeFdc {
    uint8_t bytes[4];
    uint32_t writes;
} FakeFdc;

static bool fake_read(void *user_data, uint8_t drive, uint8_t track,
    uint8_t side, uint8_t sector, uint8_t *buffer, uint32_t buffer_size,
    uint32_t *data_size)
{
    FakeFdc *fake = (FakeFdc *)user_data;
    (void)drive; (void)track; (void)side; (void)sector;
    if (buffer_size < sizeof(fake->bytes)) return false;
    memcpy(buffer, fake->bytes, sizeof(fake->bytes));
    *data_size = sizeof(fake->bytes);
    return true;
}

static bool fake_write(void *user_data, uint8_t drive, uint8_t track,
    uint8_t side, uint8_t sector, const uint8_t *buffer, uint32_t data_size)
{
    FakeFdc *fake = (FakeFdc *)user_data;
    (void)drive; (void)track; (void)side; (void)sector;
    if (data_size != sizeof(fake->bytes)) return false;
    memcpy(fake->bytes, buffer, sizeof(fake->bytes));
    fake->writes++;
    return true;
}

static void test_disk(void)
{
    uint8_t raw[40 * 5120];
    uint8_t buffer[512];
    uint8_t read_back[512];
    char error[256] = {0};
    DskImage image;
    uint32_t size = 0;
    uint8_t c, h, r, n, st1, st2;

    memset(raw, 0xA5, sizeof(raw));
    dsk_init(&image);
    CHECK(dsk_load_data(&image, raw, sizeof(raw), error, sizeof(error)));
    CHECK(dsk_drive_ready(&image, 0));
    CHECK(dsk_read_sector(&image, 0, 0, 0, 1, 2, buffer, sizeof(buffer),
        &size, &st1, &st2));
    CHECK(size == sizeof(buffer) && buffer[0] == 0xA5);
    memset(buffer, 0x3C, sizeof(buffer));
    CHECK(dsk_write_sector(&image, 0, 0, 0, 1, 2, buffer, sizeof(buffer)));
    CHECK(image.dirty);
    CHECK(dsk_read_sector(&image, 0, 0, 0, 1, 2, read_back, sizeof(read_back),
        &size, NULL, NULL) && read_back[0] == 0x3C);
    CHECK(dsk_get_sector_id(&image, 0, 0, 0, 0, &c, &h, &r, &n, &st1, &st2));
    CHECK(c == 0 && h == 0 && r == 1 && n == 2);
    CHECK(!dsk_read_sector(&image, 1, 0, 0, 1, 2, buffer, sizeof(buffer),
        &size, NULL, NULL));
    dsk_discard(&image);

    dsk_init(&image);
    CHECK(!dsk_load_data(&image, raw, 17, error, sizeof(error)));
    dsk_discard(&image);
}

static void test_mgt_layout(void)
{
    const size_t track_size = 10u * 512u;
    const size_t image_size = 40u * 2u * track_size;
    uint8_t *image_data = (uint8_t *)calloc(1, image_size);
    uint8_t buffer[512];
    uint32_t data_size = 0;
    DskImage image;
    char error[256] = {0};

    CHECK(image_data != NULL);
    if (image_data == NULL)
        return;
    for (size_t track = 0; track < 80; ++track)
        memset(image_data + track * track_size, (int)track, track_size);

    dsk_init(&image);
    CHECK(dsk_load_data(&image, image_data, image_size, error, sizeof(error)));
    CHECK(image.cylinder_count == 40 && image.side_count == 2);
    CHECK(dsk_read_sector(&image, 0, 0, 0, 1, 2, buffer, sizeof(buffer),
        &data_size, NULL, NULL) && buffer[0] == 0);
    CHECK(dsk_read_sector(&image, 0, 0, 1, 1, 2, buffer, sizeof(buffer),
        &data_size, NULL, NULL) && buffer[0] == 1);
    buffer[0] = 0xA5;
    CHECK(dsk_write_sector(&image, 0, 0, 1, 1, 2, buffer, sizeof(buffer)));
    CHECK(dsk_read_sector(&image, 0, 0, 1, 1, 2, buffer, sizeof(buffer),
        &data_size, NULL, NULL) && buffer[0] == 0xA5);
    dsk_discard(&image);
    free(image_data);
}

static void test_mgt_file_support(void)
{
    static const char *paths[] = {
        "tests/test-40s.mgt", "tests/test-40d.mgt", "tests/test-80d.mgt",
        "tests/test-side-contiguous.img"
    };
    static const size_t sizes[] = {
        40u * 10u * 512u,
        40u * 2u * 10u * 512u,
        80u * 2u * 10u * 512u,
        40u * 2u * 10u * 512u
    };
    uint8_t *data = NULL;
    uint8_t buffer[512];
    uint32_t data_size = 0;
    char error[256] = {0};

    for (size_t image_index = 0; image_index < sizeof(paths) / sizeof(paths[0]); ++image_index) {
        FILE *file = fopen(paths[image_index], "wb");
        DskImage image;
        CHECK(file != NULL);
        if (file == NULL)
            continue;
        data = (uint8_t *)calloc(1, sizes[image_index]);
        CHECK(data != NULL);
        if (data == NULL) {
            fclose(file);
            remove(paths[image_index]);
            continue;
        }
        for (size_t track = 0; track < sizes[image_index] / (10u * 512u); ++track)
            memset(data + track * 10u * 512u, (int)(track & 0xFF), 10u * 512u);
        CHECK(fwrite(data, 1, sizes[image_index], file) == sizes[image_index]);
        fclose(file);

        dsk_init(&image);
        CHECK(dsk_load_file(&image, paths[image_index], error, sizeof(error)));
        CHECK(image.inserted);
        CHECK(image.cylinder_count == (image_index == 2 ? 80 : 40));
        CHECK(image.side_count == (image_index == 0 ? 1 : 2));
        CHECK(dsk_read_sector(&image, 0, 0, 0, 1, 2, buffer, sizeof(buffer),
            &data_size, NULL, NULL));
        CHECK(data_size == sizeof(buffer));
        CHECK(buffer[0] == 0);
        if (image.side_count == 2) {
            CHECK(dsk_read_sector(&image, 0, 0, 1, 1, 2, buffer, sizeof(buffer),
                &data_size, NULL, NULL));
            CHECK(buffer[0] == (image_index == 3 ? 40 : 1));
        }
        dsk_discard(&image);
        free(data);
        data = NULL;
        remove(paths[image_index]);
    }

    {
        static const uint8_t invalid[] = {0};
        DskImage image;
        dsk_init(&image);
        CHECK(!dsk_load_data(&image, invalid, sizeof(invalid), error, sizeof(error)));
        dsk_discard(&image);
    }
}

static void test_wd1772(void)
{
    FakeFdc fake = {{1, 2, 3, 4}, 0};
    Wd1772 fdc;
    uint8_t values[4];
    wd1772_init(&fdc, fake_read, fake_write, &fake);
    wd1772_write_register(&fdc, 0, 0x80);
    CHECK((wd1772_read_register(&fdc, 0) & 3) == 3);
    for (size_t i = 0; i < sizeof(values); ++i)
        values[i] = wd1772_read_register(&fdc, 3);
    CHECK(memcmp(values, fake.bytes, sizeof(values)) == 0);
    wd1772_write_register(&fdc, 0, 0xA0);
    for (size_t i = 0; i < sizeof(values); ++i)
        wd1772_write_register(&fdc, 3, (uint8_t)(9 + i));
    CHECK(fake.writes == 1 && fake.bytes[0] == 9 && fake.bytes[3] == 12);
    wd1772_write_register(&fdc, 1, 5);
    wd1772_write_register(&fdc, 0, 0x40);
    CHECK(wd1772_read_register(&fdc, 1) == 6);
    wd1772_write_register(&fdc, 0, 0x20);
    CHECK(wd1772_read_register(&fdc, 1) == 7);
}

static void test_tape(void)
{
    uint8_t tap[27];
    size_t tap_size;
    char name[16];
    char error[256] = {0};
    TapePlayer player;
    make_tap(tap, &tap_size);
    tape_init(&player);
    CHECK(tape_load_data_with_options(&player, tap, tap_size, "test.tap",
        3500000, error, sizeof(error), NULL));
    CHECK(tape_has_tape(&player));
    CHECK(tape_first_header_name(&player, name, sizeof(name)));
    CHECK(strncmp(name, "TEST", 4) == 0);
    CHECK(tape_autoload_target(&player) == TAPE_AUTOLOAD_TARGET_48_BASIC);
    tape_start(&player, 0);
    CHECK(player.playing);
    (void)tape_input_level(&player, 1);
    tape_stop(&player);
    CHECK(!player.playing);
    tape_discard(&player);

    tape_init(&player);
    CHECK(!tape_load_data_with_options(&player, tap, 3, "bad.tap",
        3500000, error, sizeof(error), NULL));
    tape_discard(&player);
}

static void test_inflate_and_zip(void)
{
    uint8_t output[8];
    char error[256] = {0};
    ZipArchive archive;
    CHECK(!szx_inflate_zlib(NULL, 0, output, sizeof(output)));
    CHECK(!szx_inflate_raw(NULL, 0, output, sizeof(output)));
    zip_init(&archive);
    CHECK(!zip_open_file(&archive, "tests/does-not-exist.zip", error, sizeof(error)));
    zip_discard(&archive);
}

static void test_microdrive(void)
{
    MicrodriveBank bank;
    char error[256] = {0};
    const char *path = "tests/test-output.mdr";
    microdrive_bank_init(&bank);
    CHECK(microdrive_create_file(&bank, 0, path, error, sizeof(error)));
    CHECK(microdrive_ready(&bank, 0));
    CHECK(microdrive_length(&bank, 0) == MICRODRIVE_MAX_SECTORS * MICRODRIVE_SECTOR_SIZE);
    microdrive_write_byte(&bank, 0, 0, 0x5A);
    CHECK(microdrive_read_byte(&bank, 0, 0) == 0x5A);
    CHECK(microdrive_save(&bank, 0, error, sizeof(error)));
    microdrive_bank_discard(&bank);
    remove(path);
}

static void test_z80_core(void)
{
    static const uint8_t program[] = {
        0x3E, 0x7F,             /* LD A,7Fh */
        0xC6, 0x01,             /* ADD A,01h */
        0x32, 0x00, 0x80,       /* LD (8000h),A */
        0x76                    /* HALT */
    };
    uint8_t memory[65536] = {0};
    z80_t cpu;
    uint64_t pins;
    unsigned ticks = 0;

    memcpy(memory, program, sizeof(program));
    pins = z80_init(&cpu);
    while (ticks++ < 1000 && (cpu.pins & Z80_HALT) == 0)
    {
        pins = z80_tick(&cpu, pins);
        if ((pins & Z80_MREQ) != 0)
        {
            const uint16_t address = Z80_GET_ADDR(pins);
            if ((pins & Z80_RD) != 0) {
                Z80_SET_DATA(pins, memory[address]);
            } else if ((pins & Z80_WR) != 0) {
                memory[address] = Z80_GET_DATA(pins);
            }
        }
    }
    CHECK(ticks < 1000);
    CHECK((cpu.pins & Z80_HALT) != 0);
    CHECK(cpu.a == 0x80);
    CHECK((cpu.f & (Z80_SF | Z80_HF | Z80_VF)) ==
        (Z80_SF | Z80_HF | Z80_VF));
    CHECK(memory[0x8000] == 0x80);

    pins = z80_reset(&cpu);
    CHECK(cpu.pc == 0 && cpu.sp == 0xFFFF && pins == 0);
}

int main(void)
{
    dsk_init(NULL);
    dsk_discard(NULL);
    tape_init(NULL);
    tape_discard(NULL);
    microdrive_bank_init(NULL);
    microdrive_bank_discard(NULL);
    zip_init(NULL);
    zip_discard(NULL);
    test_disk();
    test_mgt_layout();
    test_mgt_file_support();
    test_wd1772();
    test_tape();
    test_inflate_and_zip();
    test_microdrive();
    test_z80_core();
    if (failures != 0) {
        fprintf(stderr, "%d test checks failed.\n", failures);
        return 1;
    }
    puts("All core tests passed.");
    return 0;
}
