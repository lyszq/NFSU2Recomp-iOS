#include "guest_memory.h"
#include <stddef.h>
uint8_t *g_mem;
uint8_t g_cbits[GUEST_MEMORY_BITMAP_SIZE];
void *guest_memory_pointer(uint32_t address, uint64_t size, int write_access) {
    (void)address; (void)size; (void)write_access;
    return NULL;
}
