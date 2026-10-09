#ifndef F3RT_BLOCK_PROFILE_H
#define F3RT_BLOCK_PROFILE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
extern uint64_t *f3_profile_main_counts;
extern uint64_t *f3_profile_sound_counts;
extern int f3_profile_indirect_enabled;
void f3_profile_indirect(uint32_t site, uint32_t target);
enum { F3_PROFILE_MAIN = 0, F3_PROFILE_SOUND = 1 };
#ifdef __cplusplus
[[noreturn]]
#else
_Noreturn
#endif
void f3_profile_cold_abort(unsigned region, uint32_t rom_crc, uint32_t address);
#ifdef __cplusplus
}
#endif
#if defined(F3_PROFILE_INSTRUMENT) && F3_PROFILE_INSTRUMENT
static inline void f3_profile_increment(uint64_t *count) {
    if (*count != UINT64_MAX) ++*count;
}
#define F3_PROFILE_HIT_MAIN(address) do { \
    if (f3_profile_main_counts) f3_profile_increment(&f3_profile_main_counts[(uint32_t)(address) >> 1]); \
} while (0)
#define F3_PROFILE_HIT_SOUND(address) do { \
    if (f3_profile_sound_counts) f3_profile_increment(&f3_profile_sound_counts[((uint32_t)(address) - 0xc00000u) >> 1]); \
} while (0)
#define F3_PROFILE_INDIRECT_MAIN(site, target) do { \
    if (f3_profile_indirect_enabled) f3_profile_indirect((uint32_t)(site), (uint32_t)(target)); \
} while (0)
#else
#define F3_PROFILE_HIT_MAIN(address) ((void)0)
#define F3_PROFILE_HIT_SOUND(address) ((void)0)
#define F3_PROFILE_INDIRECT_MAIN(site, target) ((void)0)
#endif
#endif
