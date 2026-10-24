#ifndef NEXT_DMA_H
#define NEXT_DMA_H

#include <stdbool.h>
#include <stdint.h>

struct NextMachine;

enum {
    NEXT_DMA_PENDING_CAPACITY = 8
};

typedef enum NextDmaAddrMode {
    NEXT_DMA_ADDR_DECREMENT = 0,
    NEXT_DMA_ADDR_INCREMENT = 1,
    NEXT_DMA_ADDR_FIXED = 2
} NextDmaAddrMode;

typedef enum NextDmaMode {
    NEXT_DMA_MODE_CONTINUOUS = 0,
    NEXT_DMA_MODE_BURST = 1
} NextDmaMode;

typedef enum NextDmaPendingOp {
    NEXT_DMA_OP_IGNORE = 0,
    NEXT_DMA_OP_PORT_A_LOW,
    NEXT_DMA_OP_PORT_A_HIGH,
    NEXT_DMA_OP_BLOCK_LEN_LOW,
    NEXT_DMA_OP_BLOCK_LEN_HIGH,
    NEXT_DMA_OP_PORT_B_LOW,
    NEXT_DMA_OP_PORT_B_HIGH,
    NEXT_DMA_OP_A_TIMING,
    NEXT_DMA_OP_B_TIMING,
    NEXT_DMA_OP_PRESCALER,
    NEXT_DMA_OP_READ_MASK
} NextDmaPendingOp;

/* The zxnDMA controller: the WR-register command stream on ports
   0x6B (zxn mode) and 0x0B (Z80-DMA compatible) plus the transfer
   engine. Memory/IO endpoints reach the machine through the public
   next_machine_read_memory / next_machine_read_port helpers. */
typedef struct NextDma {
    uint16_t port_a_start;
    uint16_t port_b_start;
    uint16_t block_len;
    uint16_t cur_a;
    uint16_t cur_b;
    uint16_t counter;
    bool a_to_b;
    uint8_t a_mode;
    uint8_t b_mode;
    bool a_is_io;
    bool b_is_io;
    bool z80_mode;
    uint8_t a_cycle_len;
    uint8_t b_cycle_len;
    uint8_t prescaler;
    uint8_t mode;
    bool auto_restart;
    bool end_of_block;
    bool at_least_one;
    uint64_t last_duration;
    uint64_t pending_charge;
    uint8_t read_mask;
    int read_reg;
    bool active_burst;
    bool hold_cpu;
    bool in_transfer;
    int remaining;
    uint64_t next_due;
    bool wedged;
    bool stalled;
    bool bus_delay;
    uint8_t pending[NEXT_DMA_PENDING_CAPACITY];
    unsigned pending_count;
} NextDma;

void next_dma_init(NextDma *dma);
void next_dma_reset(NextDma *dma);
void next_dma_set_z80_mode(NextDma *dma, bool on);
void next_dma_write_command(struct NextMachine *machine, uint8_t value);
uint8_t next_dma_read_command(struct NextMachine *machine);
void next_dma_step(struct NextMachine *machine, uint64_t now);
uint64_t next_dma_take_charge(NextDma *dma);

#endif
