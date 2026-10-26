/* Synthetic memory exercises the production sampler and wrappers, no game data. */
#define NFS_RUMBLE_TEST 1
#include "game_rumble.h"
#include "../ios/NFSU2iOS/Native/NFSU2InputBridge.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

uint8_t *g_mem;
uint8_t g_cbits[1u << 15];
void *guest_memory_pointer(uint32_t a, uint64_t n, int w) {
    (void)a; (void)n; (void)w; return NULL;
}
enum { VEH = 0x10000, DRIVER = 0x20000, PHYSICS = 0x30000,
       BODY = 0x40000, GFM = 0x50000, AI = 0x60000 };
static unsigned driver_calls, ff_calls;
void sub_00406460_orig(CPU *c) { ++ff_calls; c->esp += 12; }
void sub_0041e2c0_orig(CPU *c) {
    /* Simulate an update that skips FF entirely (keyboard/device gate). */
    ++driver_calls; c->eax = 123; c->ecx = 456; c->zf = 1; c->esp += 8;
}
static void setup(void) {
    memset(g_mem, 0, 0x900000);
    memset(g_cbits, 0xff, sizeof(g_cbits));
    rumble_reset_state(); nfsu2_input_clear_rumble(); nfsu2_input_set_enabled(true);
    MR32(DRIVER + 0x2a8) = VEH;
    MR32(DRIVER + 0x2ac) = UINT32_MAX; /* no native FF device */
    MR8(DRIVER + 0x2f8) = 0; /* native FF branch disabled */
    MR32(VEH + 0x28) = 1; MR32(VEH + 0x18) = 1;
    MR32(VEH + 0x2c) = PHYSICS; MR32(PHYSICS + 0x6f8) = BODY;
    MR32(0x8900d8) = GFM; MR32(GFM) = 4; MR32(0x865188) = 100;
    MRF32(VEH + 0x70) = 10;
    for (unsigned i = 0; i < 4; ++i) MR8(VEH + 0x390 + i) = 1;
}
static NFSU2RumbleSnapshot update(void) {
    CPU c = {0}; c.ecx = DRIVER; c.esp = 0x70000;
    unsigned before = driver_calls;
    sub_0041e2c0(&c);
    assert(driver_calls == before + 1);
    assert(c.eax == 123 && c.ecx == 456 && c.zf == 1 && c.esp == 0x70008);
    NFSU2RumbleSnapshot s; nfsu2_input_poll_rumble(&s); return s;
}
static void test_normal_driver_without_native_ff(void) {
    setup(); update();
    MRF32(VEH + 0x70) = 5; MR32(0x865188)++;
    NFSU2RumbleSnapshot s = update();
    assert(s.impact_intensity > 0.35f && ff_calls == 0);
    puts("PASS: normal driver produces impact with native FF disabled; CPU ABI preserved");
}
static void test_same_tick_contact_and_body(void) {
    setup(); update();
    MR32(BODY + 0x1e0) = MR32(0x865188); /* no velocity/tick change */
    NFSU2RumbleSnapshot s = update();
    assert(s.impact_intensity >= 0.45f);
    assert(update().impact_intensity == 0);
    MR32(VEH + 0x394) = 13; /* contact changes within the same tick */
    s = update(); assert(s.impact_intensity >= 0.5f && s.continuous_intensity > 0.4f);
    for (int i = 0; i < 20; ++i) assert(update().continuous_intensity > 0.4f);
    puts("PASS: correct body chain and same-tick contact, duplicate refresh without repeated impacts");
}
static void test_surfaces_slip_landing_and_gates(void) {
    setup(); MR32(VEH + 0x394) = 8;
    assert(update().continuous_intensity > 0);
    MR32(VEH + 0x394) = 0; MRF32(VEH + 0x370) = 1;
    assert(update().continuous_intensity > 0);
    MRF32(VEH + 0x370) = 0;
    for (unsigned i = 0; i < 4; ++i) MR8(VEH + 0x390 + i) = 0;
    for (unsigned i = 0; i < 7; ++i) { MR32(0x865188)++; update(); }
    MR8(VEH + 0x390) = 1;
    assert(update().impact_intensity > 0);
    MR32(VEH + 0x394) = 13; assert(update().continuous_intensity > 0);
    MR32(AI + 0x28) = 2; nfsu2_game_rumble_sample_vehicle(AI);
    NFSU2RumbleSnapshot s; nfsu2_input_poll_rumble(&s);
    assert(s.continuous_intensity > 0); /* AI cannot erase local rumble */
    MR8(0x89e7d8) = 1; s = update(); assert(s.continuous_intensity == 0);
    MR8(0x89e7d8) = 0; MR32(0x890108) = 1; s = update();
    assert(s.impact_intensity == 0 && s.continuous_intensity == 0);
    MR32(0x890108) = 0; MR32(GFM) = 0; s = update();
    assert(s.impact_intensity == 0 && s.continuous_intensity == 0);
    NFSU2RumbleDiagnostics d; nfsu2_input_get_rumble_diagnostics(&d);
    assert(d.hook_calls > 0 && d.local_samples > 0 && d.gated_samples >= 3);
    puts("PASS: surface, slip, landing, AI isolation, pause/replay/race gates and diagnostics");
}
int main(void) {
    g_mem = calloc(1, 0x900000); assert(g_mem);
    test_normal_driver_without_native_ff();
    test_same_tick_contact_and_body();
    test_surfaces_slip_landing_and_gates();
    free(g_mem); return 0;
}
