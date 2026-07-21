#include "szx_inflate.h"

#include <string.h>

enum {
    SZX_INFLATE_MAX_BITS = 15,
    SZX_INFLATE_MAX_SYMBOLS = 288,
    SZX_INFLATE_MAX_CODE_LENGTHS = 320
};

typedef struct SzxBitReader {
    const uint8_t *data;
    size_t size;
    size_t position;
    uint64_t bits;
    unsigned bit_count;
    bool failed;
} SzxBitReader;

typedef struct SzxHuffman {
    uint16_t counts[SZX_INFLATE_MAX_BITS + 1];
    uint16_t symbols[SZX_INFLATE_MAX_SYMBOLS];
} SzxHuffman;

static uint32_t szx_read_bits(SzxBitReader *reader, unsigned count)
{
    uint32_t result;
    if (count > 24)
    {
        reader->failed = true;
        return 0;
    }
    while (reader->bit_count < count)
    {
        if (reader->position >= reader->size)
        {
            reader->failed = true;
            return 0;
        }
        reader->bits |= (uint64_t)reader->data[reader->position++] << reader->bit_count;
        reader->bit_count += 8;
    }
    result = (uint32_t)(reader->bits & ((1ULL << count) - 1ULL));
    reader->bits >>= count;
    reader->bit_count -= count;
    return result;
}

static void szx_align_byte(SzxBitReader *reader)
{
    reader->bits = 0;
    reader->bit_count = 0;
}

/* Builds the canonical code tables in the reversed-bit ordering used by
   DEFLATE. An incomplete one-symbol alphabet is valid; oversubscribed trees
   are not. */
static bool szx_build_huffman(SzxHuffman *table, const uint8_t *lengths, size_t count)
{
    uint16_t offsets[SZX_INFLATE_MAX_BITS + 1] = {0};
    int available = 1;

    if (count > SZX_INFLATE_MAX_SYMBOLS)
    {
        return false;
    }
    memset(table, 0, sizeof(*table));
    for (size_t symbol = 0; symbol < count; ++symbol)
    {
        if (lengths[symbol] > SZX_INFLATE_MAX_BITS)
        {
            return false;
        }
        table->counts[lengths[symbol]]++;
    }
    if (table->counts[0] == count)
    {
        return false;
    }
    for (unsigned length = 1; length <= SZX_INFLATE_MAX_BITS; ++length)
    {
        available = (available << 1) - table->counts[length];
        if (available < 0)
        {
            return false;
        }
    }

    offsets[1] = 0;
    for (unsigned length = 1; length < SZX_INFLATE_MAX_BITS; ++length)
    {
        offsets[length + 1] = (uint16_t)(offsets[length] + table->counts[length]);
    }
    for (uint16_t symbol = 0; symbol < count; ++symbol)
    {
        const uint8_t length = lengths[symbol];
        if (length != 0)
        {
            table->symbols[offsets[length]++] = symbol;
        }
    }
    return true;
}

static int szx_decode_symbol(SzxBitReader *reader, const SzxHuffman *table)
{
    uint32_t code = 0;
    uint32_t first = 0;
    uint32_t index = 0;

    for (unsigned length = 1; length <= SZX_INFLATE_MAX_BITS; ++length)
    {
        const uint32_t count = table->counts[length];
        code |= szx_read_bits(reader, 1);
        if (reader->failed)
        {
            return -1;
        }
        if (code >= first && code - first < count)
        {
            return table->symbols[index + (code - first)];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static bool szx_build_fixed_tables(SzxHuffman *literal_table, SzxHuffman *distance_table)
{
    uint8_t literal_lengths[288];
    uint8_t distance_lengths[32];

    for (unsigned index = 0; index <= 143; ++index) literal_lengths[index] = 8;
    for (unsigned index = 144; index <= 255; ++index) literal_lengths[index] = 9;
    for (unsigned index = 256; index <= 279; ++index) literal_lengths[index] = 7;
    for (unsigned index = 280; index <= 287; ++index) literal_lengths[index] = 8;
    memset(distance_lengths, 5, sizeof(distance_lengths));
    return szx_build_huffman(literal_table, literal_lengths, 288) &&
        szx_build_huffman(distance_table, distance_lengths, 32);
}

static bool szx_build_dynamic_tables(
    SzxBitReader *reader,
    SzxHuffman *literal_table,
    SzxHuffman *distance_table)
{
    static const uint8_t order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    uint8_t code_lengths[19] = {0};
    uint8_t lengths[SZX_INFLATE_MAX_CODE_LENGTHS] = {0};
    SzxHuffman code_table;
    const unsigned literal_count = szx_read_bits(reader, 5) + 257;
    const unsigned distance_count = szx_read_bits(reader, 5) + 1;
    const unsigned code_count = szx_read_bits(reader, 4) + 4;
    const unsigned total = literal_count + distance_count;
    unsigned position = 0;

    if (reader->failed || literal_count > 286 || distance_count > 32 || total > sizeof(lengths))
    {
        return false;
    }
    for (unsigned index = 0; index < code_count; ++index)
    {
        code_lengths[order[index]] = (uint8_t)szx_read_bits(reader, 3);
    }
    if (reader->failed || !szx_build_huffman(&code_table, code_lengths, 19))
    {
        return false;
    }

    while (position < total)
    {
        const int symbol = szx_decode_symbol(reader, &code_table);
        unsigned repeat;
        uint8_t value;
        if (symbol < 0)
        {
            return false;
        }
        if (symbol <= 15)
        {
            lengths[position++] = (uint8_t)symbol;
            continue;
        }
        if (symbol == 16)
        {
            if (position == 0) return false;
            repeat = szx_read_bits(reader, 2) + 3;
            value = lengths[position - 1];
        }
        else if (symbol == 17)
        {
            repeat = szx_read_bits(reader, 3) + 3;
            value = 0;
        }
        else if (symbol == 18)
        {
            repeat = szx_read_bits(reader, 7) + 11;
            value = 0;
        }
        else
        {
            return false;
        }
        if (reader->failed || repeat > total - position)
        {
            return false;
        }
        while (repeat-- != 0)
        {
            lengths[position++] = value;
        }
    }
    if (lengths[256] == 0)
    {
        return false;
    }
    return szx_build_huffman(literal_table, lengths, literal_count) &&
        szx_build_huffman(distance_table, lengths + literal_count, distance_count);
}

static bool szx_expand_codes(
    SzxBitReader *reader,
    const SzxHuffman *literal_table,
    const SzxHuffman *distance_table,
    uint8_t *destination,
    size_t destination_size,
    size_t *destination_position)
{
    static const uint16_t length_base[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static const uint8_t length_extra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
        2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static const uint16_t distance_base[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
        6145, 8193, 12289, 16385, 24577
    };
    static const uint8_t distance_extra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };

    for (;;)
    {
        const int symbol = szx_decode_symbol(reader, literal_table);
        if (symbol < 0) return false;
        if (symbol < 256)
        {
            if (*destination_position >= destination_size) return false;
            destination[(*destination_position)++] = (uint8_t)symbol;
        }
        else if (symbol == 256)
        {
            return true;
        }
        else
        {
            unsigned length_index;
            unsigned length;
            int distance_symbol;
            unsigned distance;
            if (symbol > 285) return false;
            length_index = (unsigned)symbol - 257;
            length = length_base[length_index] + szx_read_bits(reader, length_extra[length_index]);
            distance_symbol = szx_decode_symbol(reader, distance_table);
            if (reader->failed || distance_symbol < 0 || distance_symbol > 29) return false;
            distance = distance_base[distance_symbol] +
                szx_read_bits(reader, distance_extra[distance_symbol]);
            if (reader->failed || distance > *destination_position ||
                length > destination_size - *destination_position)
            {
                return false;
            }
            while (length-- != 0)
            {
                destination[*destination_position] =
                    destination[*destination_position - distance];
                (*destination_position)++;
            }
        }
    }
}

static bool szx_expand_deflate(
    const uint8_t *source,
    size_t source_size,
    uint8_t *destination,
    size_t destination_size,
    size_t *actual_size)
{
    SzxBitReader reader = {source, source_size, 0, 0, 0, false};
    size_t destination_position = 0;
    bool final_block = false;

    while (!final_block)
    {
        const unsigned block_type = (unsigned)szx_read_bits(&reader, 1) |
            ((unsigned)szx_read_bits(&reader, 2) << 1);
        final_block = (block_type & 1u) != 0;
        switch (block_type >> 1)
        {
            case 0:
            {
                unsigned length;
                unsigned inverse_length;
                szx_align_byte(&reader);
                if (reader.position + 4 > reader.size) return false;
                length = reader.data[reader.position] | (reader.data[reader.position + 1] << 8);
                inverse_length = reader.data[reader.position + 2] |
                    (reader.data[reader.position + 3] << 8);
                reader.position += 4;
                if ((length ^ 0xFFFFu) != inverse_length ||
                    length > reader.size - reader.position ||
                    length > destination_size - destination_position)
                {
                    return false;
                }
                memcpy(destination + destination_position, reader.data + reader.position, length);
                destination_position += length;
                reader.position += length;
                break;
            }
            case 1:
            case 2:
            {
                SzxHuffman literal_table;
                SzxHuffman distance_table;
                const bool tables_ok = (block_type >> 1) == 1
                    ? szx_build_fixed_tables(&literal_table, &distance_table)
                    : szx_build_dynamic_tables(&reader, &literal_table, &distance_table);
                if (!tables_ok || !szx_expand_codes(
                        &reader, &literal_table, &distance_table,
                        destination, destination_size, &destination_position))
                {
                    return false;
                }
                break;
            }
            default:
                return false;
        }
        if (reader.failed)
        {
            return false;
        }
    }
    *actual_size = destination_position;
    return true;
}

static uint32_t szx_adler32(const uint8_t *data, size_t size)
{
    uint32_t first = 1;
    uint32_t second = 0;
    while (size != 0)
    {
        size_t chunk = size > 5552 ? 5552 : size;
        size -= chunk;
        while (chunk-- != 0)
        {
            first += *data++;
            second += first;
        }
        first %= 65521;
        second %= 65521;
    }
    return (second << 16) | first;
}

bool szx_inflate_zlib(
    const uint8_t *source,
    size_t source_size,
    uint8_t *destination,
    size_t destination_size)
{
    size_t actual_size = 0;
    if (!szx_inflate_zlib_bounded(
            source,
            source_size,
            destination,
            destination_size,
            &actual_size))
    {
        return false;
    }
    return actual_size == destination_size;
}

bool szx_inflate_zlib_bounded(
    const uint8_t *source,
    size_t source_size,
    uint8_t *destination,
    size_t destination_capacity,
    size_t *destination_size)
{
    uint16_t header;
    uint32_t expected_adler;
    size_t actual_size = 0;
    if (source == NULL || destination == NULL || destination_size == NULL ||
        source_size < 6)
    {
        return false;
    }
    header = (uint16_t)((source[0] << 8) | source[1]);
    if ((source[0] & 0x0Fu) != 8 || (source[0] >> 4) > 7 ||
        (header % 31) != 0 || (source[1] & 0x20u) != 0)
    {
        return false;
    }
    expected_adler = ((uint32_t)source[source_size - 4] << 24) |
        ((uint32_t)source[source_size - 3] << 16) |
        ((uint32_t)source[source_size - 2] << 8) |
        source[source_size - 1];
    if (!szx_expand_deflate(
            source + 2,
            source_size - 6,
            destination,
            destination_capacity,
            &actual_size) ||
        szx_adler32(destination, actual_size) != expected_adler)
    {
        return false;
    }
    *destination_size = actual_size;
    return true;
}
