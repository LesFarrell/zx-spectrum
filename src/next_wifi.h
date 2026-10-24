#ifndef NEXT_WIFI_H
#define NEXT_WIFI_H

#include <stdbool.h>
#include <stdint.h>

typedef struct NextWifi NextWifi;

NextWifi *next_wifi_create(void);
void next_wifi_destroy(NextWifi *wifi);
void next_wifi_reset(NextWifi *wifi);
void next_wifi_set_enabled(NextWifi *wifi, bool enabled);
bool next_wifi_is_enabled(NextWifi *wifi);

uint8_t next_wifi_read_status(NextWifi *wifi);
uint8_t next_wifi_read_rx(NextWifi *wifi);
void next_wifi_write_tx(NextWifi *wifi, uint8_t value);
uint8_t next_wifi_read_control(NextWifi *wifi);
void next_wifi_write_control(NextWifi *wifi, uint8_t value);
void next_wifi_write_prescaler(NextWifi *wifi, uint8_t value);
uint8_t next_wifi_read_frame(NextWifi *wifi);
void next_wifi_write_frame(NextWifi *wifi, uint8_t value);

#endif /* NEXT_WIFI_H */
