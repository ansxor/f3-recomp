#include <f3rt/cpu_abi.h>
int f3_validate_main_rom(f3_cpu *cpu, size_t size, uint32_t crc) {
    (void)cpu; (void)size; (void)crc; return 1;
}
static const f3_excluded_range *registered_exclusions;
static size_t registered_exclusion_count;
int f3_register_exclusions(f3_cpu *cpu, const f3_excluded_range *ranges, size_t count) {
    (void)cpu; registered_exclusions=ranges; registered_exclusion_count=count; return 1;
}
