#ifndef NEXT_RTC_H
#define NEXT_RTC_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* DS1307-class real-time clock wired to the Spectrum Next's bit-banged
   i2c bus.  The master drives SCL on port $103B and SDA on port $113B;
   the slave sits at i2c address $68.  Both lines are open-drain, so the
   level the master observes is the wired-AND of its own latch and the
   slave's drive.  NextZXOS polls the clock every frame to render the
   main menu's date/time line; without a slave that ACKs and drives SDA
   the fetch fails and the menu engine never settles. */
typedef enum
{
    NEXT_RTC_PHASE_ADDR = 0,
    NEXT_RTC_PHASE_ADDR_ACK,
    NEXT_RTC_PHASE_WRITE,
    NEXT_RTC_PHASE_WRITE_ACK,
    NEXT_RTC_PHASE_READ,
    NEXT_RTC_PHASE_READ_ACK,
    NEXT_RTC_PHASE_IDLE_NAK
} NextRtcPhase;

typedef struct
{
    bool scl;        /* master SCL latch (resets high) */
    bool sda_master; /* master SDA latch (resets high) */
    bool sda_slave;  /* slave SDA drive (true = released) */
    bool started;    /* transfer in progress between START and STOP */
    NextRtcPhase phase;
    int bit_count;
    uint8_t shift;
    bool reading; /* direction bit of the addressed transfer */
    bool wrote_pointer;
    uint8_t reg_pointer;
    uint8_t read_data;
    time_t clock_epoch;
    time_t host_epoch;
    uint8_t day_of_week;
    uint8_t control;
    bool clock_halted;
    bool twelve_hour;
    bool century;
    uint8_t nvram[0x40];
} NextRtc;

void next_rtc_init(NextRtc *rtc);
bool next_rtc_read_scl(const NextRtc *rtc);
bool next_rtc_read_sda(const NextRtc *rtc);
void next_rtc_write_scl(NextRtc *rtc, bool bit);
void next_rtc_write_sda(NextRtc *rtc, bool bit);

#endif /* NEXT_RTC_H */
