#include "next_rtc.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define NEXT_RTC_I2C_ADDRESS 0x68

#define NEXT_RTC_REG_SECONDS 0x00
#define NEXT_RTC_REG_MINUTES 0x01
#define NEXT_RTC_REG_HOURS 0x02
#define NEXT_RTC_REG_DAY_OF_WEEK 0x03
#define NEXT_RTC_REG_DAY_OF_MONTH 0x04
#define NEXT_RTC_REG_MONTH 0x05
#define NEXT_RTC_REG_YEAR 0x06
#define NEXT_RTC_REG_CONTROL 0x07

static uint8_t next_rtc_bcd(int value)
{
    if (value < 0)
        value = 0;
    if (value > 99)
        value = 99;
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

static int next_rtc_from_bcd(uint8_t value)
{
    return (int)((value >> 4) & 0x0F) * 10 + (int)(value & 0x0F);
}

static bool next_rtc_current_time(const NextRtc *rtc, struct tm *value)
{
    time_t epoch = rtc->clock_epoch;
    if (!rtc->clock_halted)
        epoch += time(NULL) - rtc->host_epoch;
    {
        const struct tm *local = localtime(&epoch);
        if (local == NULL)
            return false;
        *value = *local;
    }
    return true;
}

static uint8_t next_rtc_read_register(const NextRtc *rtc, uint8_t reg)
{
    struct tm current;
    if (!next_rtc_current_time(rtc, &current))
        return 0;
    switch (reg)
    {
    case NEXT_RTC_REG_SECONDS:
        return (uint8_t)(next_rtc_bcd(current.tm_sec) |
                         (rtc->clock_halted ? 0x80 : 0));
    case NEXT_RTC_REG_MINUTES:
        return next_rtc_bcd(current.tm_min);
    case NEXT_RTC_REG_HOURS:
        if (rtc->twelve_hour)
        {
            const int hour = current.tm_hour % 12 == 0 ? 12 : current.tm_hour % 12;
            return (uint8_t)(0x40 | next_rtc_bcd(hour) |
                             (current.tm_hour >= 12 ? 0x20 : 0));
        }
        return next_rtc_bcd(current.tm_hour);
    case NEXT_RTC_REG_DAY_OF_WEEK:
        return rtc->day_of_week;
    case NEXT_RTC_REG_DAY_OF_MONTH:
        return next_rtc_bcd(current.tm_mday);
    case NEXT_RTC_REG_MONTH:
        return (uint8_t)(next_rtc_bcd(current.tm_mon + 1) |
                         (rtc->century ? 0x80 : 0));
    case NEXT_RTC_REG_YEAR:
        return next_rtc_bcd((current.tm_year + 1900) % 100);
    case NEXT_RTC_REG_CONTROL:
        return rtc->control;
    default:
        break;
    }
    if (reg < 0x40)
        return rtc->nvram[reg];
    return 0;
}

static void next_rtc_write_register(NextRtc *rtc, uint8_t reg, uint8_t value)
{
    struct tm current;
    time_t host_now;
    if (reg == NEXT_RTC_REG_CONTROL)
    {
        rtc->control = value;
        return;
    }
    if (reg >= 0x08 && reg < 0x40)
    {
        rtc->nvram[reg] = value;
        return;
    }
    if (reg == NEXT_RTC_REG_DAY_OF_WEEK)
    {
        rtc->day_of_week = value & 0x07;
        return;
    }
    if (reg > NEXT_RTC_REG_YEAR || !next_rtc_current_time(rtc, &current))
        return;
    switch (reg)
    {
    case NEXT_RTC_REG_SECONDS:
        rtc->clock_halted = (value & 0x80) != 0;
        current.tm_sec = next_rtc_from_bcd(value & 0x7F);
        break;
    case NEXT_RTC_REG_MINUTES:
        current.tm_min = next_rtc_from_bcd(value & 0x7F);
        break;
    case NEXT_RTC_REG_HOURS:
        rtc->twelve_hour = (value & 0x40) != 0;
        if (rtc->twelve_hour)
        {
            int hour = next_rtc_from_bcd(value & 0x1F) % 12;
            if ((value & 0x20) != 0)
                hour += 12;
            current.tm_hour = hour;
        }
        else
        {
            current.tm_hour = next_rtc_from_bcd(value & 0x3F);
        }
        break;
    case NEXT_RTC_REG_DAY_OF_MONTH:
        current.tm_mday = next_rtc_from_bcd(value & 0x3F);
        break;
    case NEXT_RTC_REG_MONTH:
        rtc->century = (value & 0x80) != 0;
        current.tm_mon = next_rtc_from_bcd(value & 0x1F) - 1;
        break;
    case NEXT_RTC_REG_YEAR:
        current.tm_year = next_rtc_from_bcd(value) +
                          (rtc->century ? 200 : 100);
        break;
    default:
        return;
    }
    host_now = time(NULL);
    current.tm_isdst = -1;
    {
        const time_t updated = mktime(&current);
        if (updated != (time_t)-1)
        {
            rtc->clock_epoch = updated;
            rtc->host_epoch = host_now;
        }
    }
}

void next_rtc_init(NextRtc *rtc)
{
    const time_t now = time(NULL);
    const struct tm *local = localtime(&now);
    rtc->scl = true;
    rtc->sda_master = true;
    rtc->sda_slave = true;
    rtc->started = false;
    rtc->phase = NEXT_RTC_PHASE_IDLE_NAK;
    rtc->bit_count = 0;
    rtc->shift = 0;
    rtc->reading = false;
    rtc->wrote_pointer = false;
    rtc->reg_pointer = 0;
    rtc->read_data = 0;
    rtc->clock_epoch = now;
    rtc->host_epoch = now;
    rtc->day_of_week = local != NULL ? (uint8_t)(local->tm_wday + 1) : 1;
    rtc->control = 0;
    rtc->clock_halted = false;
    rtc->twelve_hour = false;
    rtc->century = false;
}

bool next_rtc_read_sda(const NextRtc *rtc)
{
    return rtc->sda_master && rtc->sda_slave;
}

void next_rtc_write_sda(NextRtc *rtc, bool bit)
{
    const bool previous = rtc->sda_master && rtc->sda_slave;
    rtc->sda_master = bit;
    const bool now = rtc->sda_master && rtc->sda_slave;
    if (!rtc->scl || previous == now)
        return;
    if (!now)
    {
        /* Falling edge while SCL is high is a START (or repeated START). */
        rtc->started = true;
        rtc->phase = NEXT_RTC_PHASE_ADDR;
        rtc->bit_count = 0;
        rtc->shift = 0;
        rtc->sda_slave = true;
        return;
    }
    /* Rising edge while SCL is high is a STOP. */
    rtc->started = false;
    rtc->sda_slave = true;
}

static void next_rtc_drive_read_bit(NextRtc *rtc)
{
    rtc->sda_slave = (rtc->read_data & 0x80) != 0;
    rtc->read_data = (uint8_t)(rtc->read_data << 1);
}

static void next_rtc_rising_scl(NextRtc *rtc)
{
    switch (rtc->phase)
    {
    case NEXT_RTC_PHASE_ADDR:
    case NEXT_RTC_PHASE_WRITE:
        rtc->shift = (uint8_t)(rtc->shift << 1);
        if (rtc->sda_master && rtc->sda_slave)
            rtc->shift |= 1;
        rtc->bit_count++;
        break;
    case NEXT_RTC_PHASE_READ:
        rtc->bit_count++;
        break;
    case NEXT_RTC_PHASE_READ_ACK:
        if (rtc->sda_master && rtc->sda_slave)
        {
            rtc->phase = NEXT_RTC_PHASE_IDLE_NAK;
        }
        else
        {
            rtc->reg_pointer = (uint8_t)((rtc->reg_pointer + 1) & 0x3F);
            rtc->phase = NEXT_RTC_PHASE_READ;
            rtc->bit_count = -1;
        }
        break;
    default:
        break;
    }
}

static void next_rtc_falling_scl(NextRtc *rtc)
{
    switch (rtc->phase)
    {
    case NEXT_RTC_PHASE_ADDR:
        if (rtc->bit_count < 8)
            return;
        if ((rtc->shift >> 1) == NEXT_RTC_I2C_ADDRESS)
        {
            rtc->reading = (rtc->shift & 1) != 0;
            rtc->sda_slave = false;
            rtc->phase = NEXT_RTC_PHASE_ADDR_ACK;
        }
        else
        {
            rtc->sda_slave = true;
            rtc->phase = NEXT_RTC_PHASE_IDLE_NAK;
        }
        break;
    case NEXT_RTC_PHASE_ADDR_ACK:
        rtc->sda_slave = true;
        rtc->bit_count = 0;
        rtc->shift = 0;
        if (rtc->reading)
        {
            rtc->read_data = next_rtc_read_register(rtc, rtc->reg_pointer);
            rtc->phase = NEXT_RTC_PHASE_READ;
            next_rtc_drive_read_bit(rtc);
        }
        else
        {
            rtc->phase = NEXT_RTC_PHASE_WRITE;
            rtc->wrote_pointer = false;
        }
        break;
    case NEXT_RTC_PHASE_WRITE:
        if (rtc->bit_count < 8)
            return;
        if (!rtc->wrote_pointer)
        {
            rtc->reg_pointer = (uint8_t)(rtc->shift & 0x3F);
            rtc->wrote_pointer = true;
        }
        else
        {
            next_rtc_write_register(rtc, rtc->reg_pointer, rtc->shift);
            rtc->reg_pointer = (uint8_t)((rtc->reg_pointer + 1) & 0x3F);
        }
        rtc->sda_slave = false;
        rtc->phase = NEXT_RTC_PHASE_WRITE_ACK;
        break;
    case NEXT_RTC_PHASE_WRITE_ACK:
        rtc->sda_slave = true;
        rtc->bit_count = 0;
        rtc->shift = 0;
        rtc->phase = NEXT_RTC_PHASE_WRITE;
        break;
    case NEXT_RTC_PHASE_READ:
        if (rtc->bit_count == -1)
        {
            rtc->read_data = next_rtc_read_register(rtc, rtc->reg_pointer);
            rtc->bit_count = 0;
            next_rtc_drive_read_bit(rtc);
            return;
        }
        if (rtc->bit_count < 8)
        {
            next_rtc_drive_read_bit(rtc);
            return;
        }
        rtc->sda_slave = true;
        rtc->phase = NEXT_RTC_PHASE_READ_ACK;
        break;
    case NEXT_RTC_PHASE_IDLE_NAK:
    default:
        rtc->sda_slave = true;
        break;
    }
}

void next_rtc_write_scl(NextRtc *rtc, bool bit)
{
    if (bit == rtc->scl)
        return;
    rtc->scl = bit;
    if (!rtc->started)
        return;
    if (bit)
        next_rtc_rising_scl(rtc);
    else
        next_rtc_falling_scl(rtc);
}
