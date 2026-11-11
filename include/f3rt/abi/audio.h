#ifndef F3RT_ABI_AUDIO_H
#define F3RT_ABI_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The F3 sound board, one per process. It is driven by main-CPU mailbox writes and renders
 * interleaved stereo (left, right) at f3rt_audio_sample_rate() on its own synth worker thread.
 * Every function except init and shutdown requires an initialized instance. Call all of them
 * from one thread. A failure inside the board (synth worker error, exhausted command IDs, use
 * before init) prints a message and aborts the process. */

typedef int32_t f3rt_audio_status;
#define F3RT_AUDIO_OK 0
#define F3RT_AUDIO_UNSUPPORTED_ROM 1
#define F3RT_AUDIO_ALREADY_INITIALIZED 2
#define F3RT_AUDIO_INVALID_STATE 3

/* Size of the save-state blob written by f3rt_audio_save_state. */
#define F3RT_AUDIO_STATE_SIZE 32u

typedef struct f3rt_audio_stats {
    uint64_t commands, rendered_frames, voices_started, render_ns;
    uint32_t voices_active;
} f3rt_audio_stats;

/* sound_rom: 68000 program, at least 0x80000 bytes (interleaved high/low byte pairs).
 * sample_rom: big-endian 16-bit sample ROM bytes. Both are copied.
 * shared_ram: the 0x800-byte dual-port RAM owned by the main CPU bus, borrowed until shutdown;
 * the board reads mailbox packets from it and writes the consumer index back.
 * Returns UNSUPPORTED_ROM when the program or samples cannot be driven, ALREADY_INITIALIZED
 * when an instance exists. */
f3rt_audio_status f3rt_audio_init(const uint8_t *sound_rom, size_t sound_rom_size,
                                  const uint8_t *sample_rom, size_t sample_rom_size,
                                  uint8_t *shared_ram);
/* Stops the worker and releases the instance. No effect when none exists. */
void f3rt_audio_shutdown(void);

/* The main CPU wrote shared RAM at `offset` (0..0x7ff). */
void f3rt_audio_shared_write(uint32_t offset);
/* Sound CPU reset line, active high: asserted holds the board. Asserting it is also the
 * whole-machine and watchdog reset; the continuous audio clock is kept. */
void f3rt_audio_set_reset(bool asserted);
/* Sound program ROM bank for the 0xc20000-0xc3ffff window; page 1 after init. */
void f3rt_audio_set_sound_bank(uint32_t page);
/* Advances board time by main-CPU cycles. */
void f3rt_audio_advance(uint32_t main_cycles);
/* Closes the video frame's audio: submits pending time and marks the frame boundary. */
void f3rt_audio_finish_frame(void);

uint32_t f3rt_audio_sample_rate(void);
/* Blocking: waits for the worker to finish all submitted time, then drains up to max_frames. */
size_t f3rt_audio_render(int16_t *interleaved_stereo, size_t max_frames);
/* Non-blocking: drains only PCM already produced, holding back the frame f3rt_audio_finish_frame
 * just closed, so the amount never races the worker. Callers that need a complete,
 * deterministic stream finish with f3rt_audio_render. */
size_t f3rt_audio_render_ready(int16_t *interleaved_stereo, size_t max_frames);
f3rt_audio_stats f3rt_audio_performance_stats(void);

/* dst and src hold F3RT_AUDIO_STATE_SIZE bytes. load_state returns INVALID_STATE for a
 * malformed blob and leaves the board unchanged. */
void f3rt_audio_save_state(uint8_t *dst);
f3rt_audio_status f3rt_audio_load_state(const uint8_t *src);

#ifdef __cplusplus
}
#endif
#endif
