#ifndef F3RT_ABI_EEPROM_H
#define F3RT_ABI_EEPROM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 93C46 serial EEPROM (64 x 16-bit words), one per process. Every function except init and
 * shutdown requires an initialized instance. Internal failures abort. */

typedef int32_t f3rt_eeprom_status;
#define F3RT_EEPROM_OK 0
#define F3RT_EEPROM_ALREADY_INITIALIZED 1
#define F3RT_EEPROM_INVALID_STATE 2
#define F3RT_EEPROM_INVALID_FILE 3
#define F3RT_EEPROM_IO_ERROR 4

#define F3RT_EEPROM_WORDS 64u
#define F3RT_EEPROM_FILE_BYTES 128u
#define F3RT_EEPROM_STATE_SIZE 157u

/* Initializes the EEPROM with all 64 words set to 0xffff. */
f3rt_eeprom_status f3rt_eeprom_init(void);
void f3rt_eeprom_shutdown(void);

/* Seeds the 64 words from a 128-byte big-endian factory image. */
void f3rt_eeprom_seed_be_bytes(const uint8_t *bytes);

/* Resets serial-interface state; keeps the stored words. */
void f3rt_eeprom_reset(void);
/* Drives CS/CLK/DI (bits 4/3/2 of `pins`) at main-clock tick `now`. */
void f3rt_eeprom_pins(uint8_t pins, uint64_t now);
/* DO pin level at main-clock tick `now` (reports ready/busy when CS is polled). */
bool f3rt_eeprom_output(uint64_t now);

/* 128-byte big-endian file IO. A missing file on load is a no-op returning OK. */
f3rt_eeprom_status f3rt_eeprom_load_file(const char *path);
f3rt_eeprom_status f3rt_eeprom_save_file(const char *path);

/* dst and src hold F3RT_EEPROM_STATE_SIZE bytes (CanonicalEeprom layout, host little-endian). */
void f3rt_eeprom_save_state(uint8_t *dst);
f3rt_eeprom_status f3rt_eeprom_load_state(const uint8_t *src);

#ifdef __cplusplus
}
#endif
#endif
