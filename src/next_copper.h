#ifndef NEXT_COPPER_H
#define NEXT_COPPER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct NextMachine;

enum {
    NEXT_COPPER_MAX_INSTRUCTIONS = 1024,
    NEXT_COPPER_RAM_BYTES = NEXT_COPPER_MAX_INSTRUCTIONS * 2,
    NEXT_COPPER_CLOCKS_PER_HCOUNT = 4,
    NEXT_COPPER_HCOUNT_ORIGIN = 12
};

typedef enum NextCopperOp {
    NEXT_COPPER_OP_NOOP = 0,
    NEXT_COPPER_OP_MOVE,
    NEXT_COPPER_OP_WAIT
} NextCopperOp;

typedef enum NextCopperStartMode {
    NEXT_COPPER_START_STOP = 0,
    NEXT_COPPER_START_FROM_ZERO = 1,
    NEXT_COPPER_START_CONTINUE = 2,
    NEXT_COPPER_START_ON_VBL = 3
} NextCopperStartMode;

typedef struct NextCopperInstruction {
    NextCopperOp op;
    uint8_t reg;
    uint8_t value;
    uint16_t y;
    uint8_t x;
} NextCopperInstruction;

typedef struct NextCopper {
    uint16_t program[NEXT_COPPER_MAX_INSTRUCTIONS];
    uint16_t write_ptr;
    uint8_t mode;
    bool restart_pending;
    uint16_t pc;
    bool stopped;
    uint16_t last_scanline;
} NextCopper;

void next_copper_init(NextCopper *copper);
void next_copper_reset(NextCopper *copper);
void next_copper_write_data(NextCopper *copper, uint8_t value);
void next_copper_set_write_ptr_low(NextCopper *copper, uint8_t value);
void next_copper_set_write_ptr_high_and_mode(NextCopper *copper, uint8_t value);
bool next_copper_idle(const NextCopper *copper);
int next_copper_step(
    struct NextMachine *machine,
    NextCopper *copper,
    uint16_t scanline,
    uint16_t hcount,
    int max_clocks
);

#endif
