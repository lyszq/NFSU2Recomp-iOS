/*
 * Test suite for NFSU2InputBridge (DirectInput DIK_* key table & analog mapping)
 */

#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include "../ios/NFSU2iOS/Native/NFSU2InputBridge.h"
#include "guest_cursor.h"
#include "guest_memory.h"

/* Guest memory emulation for standalone test runner */
uint8_t *g_mem = NULL;
uint8_t g_cbits[GUEST_MEMORY_BITMAP_SIZE];

void *guest_memory_pointer(uint32_t address, uint64_t size, int write_access) {
    (void)address; (void)size; (void)write_access;
    return NULL;
}

static void test_initial_and_clear(void) {
    nfsu2_input_clear_all();
    assert(nfsu2_input_active_count() == 0);
    
    uint8_t kb[256];
    memset(kb, 0xFF, sizeof(kb));
    nfsu2_input_fill_keyboard(kb, 256);
    for (int i = 0; i < 256; i++) {
        assert(kb[i] == 0x00);
    }
    printf("PASS: test_initial_and_clear\n");
}

static void test_key_set_and_get(void) {
    nfsu2_input_clear_all();
    
    // Set gas and NOS
    nfsu2_input_set_key(NFSU2_DIK_UP, true);
    nfsu2_input_set_key(NFSU2_DIK_LSHIFT, true);
    
    assert(nfsu2_input_get_key(NFSU2_DIK_UP) == true);
    assert(nfsu2_input_get_key(NFSU2_DIK_LSHIFT) == true);
    assert(nfsu2_input_get_key(NFSU2_DIK_DOWN) == false);
    assert(nfsu2_input_active_count() == 2);
    
    uint8_t kb[256];
    nfsu2_input_fill_keyboard(kb, 256);
    assert(kb[NFSU2_DIK_UP] == 0x80);
    assert(kb[NFSU2_DIK_LSHIFT] == 0x80);
    assert(kb[NFSU2_DIK_DOWN] == 0x00);
    
    // Release NOS
    nfsu2_input_set_key(NFSU2_DIK_LSHIFT, false);
    assert(nfsu2_input_get_key(NFSU2_DIK_LSHIFT) == false);
    assert(nfsu2_input_active_count() == 1);
    
    printf("PASS: test_key_set_and_get\n");
}

static void test_steering_analog(void) {
    nfsu2_input_clear_all();
    
    // Hard left: -1.0
    nfsu2_input_set_steering(-1.0f);
    assert(nfsu2_input_get_key(NFSU2_DIK_LEFT) == true);
    assert(nfsu2_input_get_key(NFSU2_DIK_A) == true);
    assert(nfsu2_input_get_key(NFSU2_DIK_RIGHT) == false);
    assert(nfsu2_input_get_key(NFSU2_DIK_D) == false);
    
    // Hard right: +1.0
    nfsu2_input_set_steering(1.0f);
    assert(nfsu2_input_get_key(NFSU2_DIK_LEFT) == false);
    assert(nfsu2_input_get_key(NFSU2_DIK_A) == false);
    assert(nfsu2_input_get_key(NFSU2_DIK_RIGHT) == true);
    assert(nfsu2_input_get_key(NFSU2_DIK_D) == true);
    
    // Deadzone: 0.0
    nfsu2_input_set_steering(0.0f);
    assert(nfsu2_input_get_key(NFSU2_DIK_LEFT) == false);
    assert(nfsu2_input_get_key(NFSU2_DIK_A) == false);
    assert(nfsu2_input_get_key(NFSU2_DIK_RIGHT) == false);
    assert(nfsu2_input_get_key(NFSU2_DIK_D) == false);
    
    printf("PASS: test_steering_analog\n");
}

static void test_pedals_analog(void) {
    nfsu2_input_clear_all();
    
    // Throttle press
    nfsu2_input_set_throttle(0.85f);
    assert(nfsu2_input_get_key(NFSU2_DIK_UP) == true);
    assert(nfsu2_input_get_key(NFSU2_DIK_W) == true);
    
    // Throttle release
    nfsu2_input_set_throttle(0.02f);
    assert(nfsu2_input_get_key(NFSU2_DIK_UP) == false);
    assert(nfsu2_input_get_key(NFSU2_DIK_W) == false);
    
    // Brake press
    nfsu2_input_set_brake(0.90f);
    assert(nfsu2_input_get_key(NFSU2_DIK_DOWN) == true);
    assert(nfsu2_input_get_key(NFSU2_DIK_S) == true);
    
    // Brake release
    nfsu2_input_set_brake(0.0f);
    assert(nfsu2_input_get_key(NFSU2_DIK_DOWN) == false);
    assert(nfsu2_input_get_key(NFSU2_DIK_S) == false);
    
    printf("PASS: test_pedals_analog\n");
}

static void test_metal_fill_keyboard_compat(void) {
    nfsu2_input_clear_all();
    nfsu2_input_set_key(NFSU2_DIK_SPACE, true);
    nfsu2_input_set_key(NFSU2_DIK_ESCAPE, true);
    
    unsigned char buf[256];
    memset(buf, 0, sizeof(buf));
    metal_fill_keyboard(buf, 256);
    
    assert(buf[NFSU2_DIK_SPACE] == 0x80);
    assert(buf[NFSU2_DIK_ESCAPE] == 0x80);
    assert(buf[NFSU2_DIK_RETURN] == 0x00);
    
    printf("PASS: test_metal_fill_keyboard_compat\n");
}

static void test_metal_available(void) {
    assert(metal_available() == 1);
    printf("PASS: test_metal_available\n");
}

static void test_cursor_bridge(void) {
    nfsu2_input_set_viewport(1280, 720, 1280, 720);
    nfsu2_input_set_cursor(640.0f, 360.0f, false);

    int cx = 0, cy = 0;
    bool avail = nfsu2_input_get_cursor(&cx, &cy);
    assert(avail == true);
    assert(cx == 640);
    assert(cy == 360);

    /* Test viewport aspect scaling with letterboxing: 1920x1080 viewport, 640x480 (4:3) game */
    nfsu2_input_set_viewport(1920, 1080, 640, 480);
    /* Center of viewport: 960, 540 should map to game center: 320, 240 */
    nfsu2_input_set_cursor(960.0f, 540.0f, true);
    avail = nfsu2_input_get_cursor(&cx, &cy);
    assert(avail == true);
    assert(cx == 320);
    assert(cy == 240);

    /* Test availability toggle */
    nfsu2_input_set_cursor_available(false);
    avail = nfsu2_input_get_cursor(&cx, &cy);
    assert(avail == false);

    printf("PASS: test_cursor_bridge\n");
}

static void test_cursor_snapshot_bridge(void) {
    guest_cursor_reset();

    int32_t x = -1, y = -1;
    uint32_t flags = UINT32_MAX;
    bool available = nfsu2_input_get_cursor_snapshot(&x, &y, &flags);
    assert(!available);
    assert((flags & GUEST_CURSOR_FLAG_AVAILABLE) == 0);
    assert((flags & GUEST_CURSOR_FLAG_DOWN) == 0);

    nfsu2_input_set_viewport(1280, 720, 1280, 720);
    nfsu2_input_set_cursor(640.0f, 360.0f, true);
    available = nfsu2_input_get_cursor_snapshot(&x, &y, &flags);
    assert(available);
    assert(x == 640 && y == 360);
    assert((flags & GUEST_CURSOR_FLAG_AVAILABLE) != 0);
    assert((flags & GUEST_CURSOR_FLAG_DOWN) != 0);
    assert((flags & GUEST_CURSOR_FLAG_SYNTHETIC) == 0);

    /* The deterministic 320,180 fixture is explicit and must not replace live state. */
    const uint32_t fixture_flags = GUEST_CURSOR_FLAG_AVAILABLE |
                                   GUEST_CURSOR_FLAG_DOWN |
                                   GUEST_CURSOR_FLAG_SYNTHETIC;
    guest_cursor_inject_synthetic(320, 180, fixture_flags);
    available = nfsu2_input_get_cursor_snapshot(&x, &y, &flags);
    assert(available);
    assert(x == 320 && y == 180);
    assert(flags == fixture_flags);

    guest_cursor_inject_synthetic(320, 180, GUEST_CURSOR_FLAG_SYNTHETIC);
    available = nfsu2_input_get_cursor_snapshot(&x, &y, &flags);
    assert(!available);
    assert(x == 320 && y == 180);
    assert(flags == GUEST_CURSOR_FLAG_SYNTHETIC);

    guest_cursor_clear_synthetic();
    available = nfsu2_input_get_cursor_snapshot(&x, &y, &flags);
    assert(available);
    assert(x == 640 && y == 360);
    assert((flags & GUEST_CURSOR_FLAG_DOWN) != 0);

    nfsu2_input_set_cursor_available(false);
    available = nfsu2_input_get_cursor_snapshot(&x, &y, &flags);
    assert(!available);
    assert((flags & GUEST_CURSOR_FLAG_AVAILABLE) == 0);
    assert((flags & GUEST_CURSOR_FLAG_DOWN) == 0);
    printf("PASS: test_cursor_snapshot_bridge\n");
}

static void test_metal_fill_mouse_bridge(void) {
    guest_cursor_reset();
    nfsu2_input_set_viewport(1280, 720, 1280, 720);
    nfsu2_input_set_cursor(100.0f, 100.0f, false);

    unsigned char buf[20];
    metal_fill_mouse(buf, 20);

    /* Move cursor with touch down */
    nfsu2_input_set_cursor(130.0f, 150.0f, true);
    metal_fill_mouse(buf, 20);

    int32_t dx = 0, dy = 0;
    memcpy(&dx, buf + 0, 4);
    memcpy(&dy, buf + 4, 4);
    assert(dx == 30);
    assert(dy == 50);
    assert(buf[12] == 0x80);

    /* Consecutive call resets deltas */
    metal_fill_mouse(buf, 20);
    memcpy(&dx, buf + 0, 4);
    memcpy(&dy, buf + 4, 4);
    assert(dx == 0);
    assert(dy == 0);

    printf("PASS: test_metal_fill_mouse_bridge\n");
}

static void test_rumble_initial_and_null_safety(void) {
    nfsu2_input_clear_rumble();
    assert(nfsu2_input_poll_rumble(NULL) == false);

    NFSU2RumbleSnapshot snap;
    memset(&snap, 0xFF, sizeof(snap));
    bool active = nfsu2_input_poll_rumble(&snap);
    assert(active == false);
    assert(snap.continuous_intensity == 0.0f);
    assert(snap.continuous_sharpness == 0.0f);
    assert(snap.impact_intensity == 0.0f);
    assert(snap.impact_sharpness == 0.0f);
    assert(snap.impact_duration_ms == 0);

    printf("PASS: test_rumble_initial_and_null_safety\n");
}

static void test_rumble_continuous_and_ttl(void) {
    nfsu2_input_clear_rumble();

    /* Raporla: %60 güç, %40 keskinlik */
    nfsu2_input_report_rumble(0.6f, 0.4f);

    NFSU2RumbleSnapshot snap;
    /* TTL = 8 olduğu için 8 kez poll çağrısında aktif kalmalıdır */
    for (int i = 0; i < 8; i++) {
        bool active = nfsu2_input_poll_rumble(&snap);
        assert(active == true);
        assert(fabsf(snap.continuous_intensity - 0.6f) < 0.005f);
        assert(fabsf(snap.continuous_sharpness - 0.4f) < 0.005f);
        assert(snap.impact_intensity == 0.0f);
    }

    /* 9. poll çağrısında TTL 0'a düşmüş olmalı ve sönümlenmelidir */
    bool active = nfsu2_input_poll_rumble(&snap);
    assert(active == false);
    assert(snap.continuous_intensity == 0.0f);

    /* Clamp testi */
    nfsu2_input_report_rumble(1.5f, -0.5f);
    active = nfsu2_input_poll_rumble(&snap);
    assert(active == true);
    assert(fabsf(snap.continuous_intensity - 1.0f) < 0.005f);
    assert(snap.continuous_sharpness == 0.0f);

    /* 0.001f altındaki genlik hemen sıfırlanmalı ve TTL 0 olmalıdır */
    nfsu2_input_report_rumble(0.0005f, 0.5f);
    active = nfsu2_input_poll_rumble(&snap);
    assert(active == false);
    assert(snap.continuous_intensity == 0.0f);

    printf("PASS: test_rumble_continuous_and_ttl\n");
}

static void test_rumble_impact_trigger_and_consumption(void) {
    nfsu2_input_clear_rumble();

    /* Geçerli darbe tetikle */
    nfsu2_input_trigger_impact(0.85f, 0.65f, 150u);

    NFSU2RumbleSnapshot snap;
    bool active = nfsu2_input_poll_rumble(&snap);
    assert(active == true);
    assert(fabsf(snap.impact_intensity - 0.85f) < 0.005f);
    assert(fabsf(snap.impact_sharpness - 0.65f) < 0.005f);
    assert(snap.impact_duration_ms == 150u);
    assert(snap.impact_id > 0);
    uint32_t last_id = snap.impact_id;

    /* İkinci poll çağrısında tüketilmiş olmalı (impact_intensity 0 döner, ID korunur) */
    active = nfsu2_input_poll_rumble(&snap);
    assert(active == false);
    assert(snap.impact_intensity == 0.0f);
    assert(snap.impact_duration_ms == 0);
    assert(snap.impact_id == last_id);

    /* Süre clamp testleri (60..280 ms) */
    nfsu2_input_trigger_impact(0.5f, 0.5f, 20u); /* < 60 olamaz */
    nfsu2_input_poll_rumble(&snap);
    assert(snap.impact_duration_ms == 60u);

    nfsu2_input_trigger_impact(0.5f, 0.5f, 500u); /* > 280 olamaz */
    nfsu2_input_poll_rumble(&snap);
    assert(snap.impact_duration_ms == 280u);

    /* <= 0.01f darbe yok sayılır */
    last_id = snap.impact_id;
    nfsu2_input_trigger_impact(0.008f, 0.5f, 100u);
    active = nfsu2_input_poll_rumble(&snap);
    assert(active == false);
    assert(snap.impact_id == last_id);

    printf("PASS: test_rumble_impact_trigger_and_consumption\n");
}

static void test_rumble_impact_coalescing(void) {
    nfsu2_input_clear_rumble();

    /* İlk darbe */
    nfsu2_input_trigger_impact(0.40f, 0.30f, 100u);

    NFSU2RumbleSnapshot snap;
    /* Henüz poll edilmedi (Swift tüketmedi). Daha zayıf bir darbe gönder */
    nfsu2_input_trigger_impact(0.20f, 0.90f, 220u);

    /* Tüketim öncesi daha güçlü bir darbe gönder (coalescing: güçlü olan ezmelidir) */
    nfsu2_input_trigger_impact(0.75f, 0.60f, 180u);

    bool active = nfsu2_input_poll_rumble(&snap);
    assert(active == true);
    assert(fabsf(snap.impact_intensity - 0.75f) < 0.005f);
    assert(fabsf(snap.impact_sharpness - 0.60f) < 0.005f);
    assert(snap.impact_duration_ms == 180u);

    printf("PASS: test_rumble_impact_coalescing\n");
}

static void test_rumble_clear_and_disabled(void) {
    nfsu2_input_clear_rumble();

    /* Rumble aktif iken clear_rumble çağrısı */
    nfsu2_input_report_rumble(0.9f, 0.8f);
    nfsu2_input_trigger_impact(0.8f, 0.7f, 120u);
    nfsu2_input_clear_rumble();

    NFSU2RumbleSnapshot snap;
    bool active = nfsu2_input_poll_rumble(&snap);
    assert(active == false);
    assert(snap.continuous_intensity == 0.0f);
    assert(snap.impact_intensity == 0.0f);

    /* Controller clear sırasında rumble da sıfırlanmalı */
    nfsu2_input_report_rumble(0.9f, 0.8f);
    nfsu2_input_clear_controller();
    active = nfsu2_input_poll_rumble(&snap);
    assert(active == false);
    assert(snap.continuous_intensity == 0.0f);

    /* input_set_enabled(false) durumu */
    nfsu2_input_report_rumble(0.7f, 0.7f);
    nfsu2_input_trigger_impact(0.8f, 0.8f, 100u);
    nfsu2_input_set_enabled(false);

    active = nfsu2_input_poll_rumble(&snap);
    assert(active == false);
    assert(snap.continuous_intensity == 0.0f);
    assert(snap.impact_intensity == 0.0f);

    /* Devre dışıyken gelen çağrılar yoksayılmalı */
    nfsu2_input_report_rumble(0.9f, 0.9f);
    nfsu2_input_trigger_impact(0.9f, 0.9f, 100u);
    active = nfsu2_input_poll_rumble(&snap);
    assert(active == false);

    /* Yeniden aktif edildiğinde sıfırlanmış olmalı ve yeni girdiler kabul edilmeli */
    nfsu2_input_set_enabled(true);
    active = nfsu2_input_poll_rumble(&snap);
    assert(active == false);

    nfsu2_input_report_rumble(0.5f, 0.5f);
    active = nfsu2_input_poll_rumble(&snap);
    assert(active == true);

    /* nfsu2_input_reset() testi */
    nfsu2_input_reset();
    active = nfsu2_input_poll_rumble(&snap);
    assert(active == false);
    assert(snap.continuous_intensity == 0.0f);
    assert(snap.impact_intensity == 0.0f);

    printf("PASS: test_rumble_clear_and_disabled\n");
}

static void test_background_foreground_both_sources(void) {
    for (unsigned cycle = 0; cycle < 50; ++cycle) {
        nfsu2_input_set_enabled(true);
        nfsu2_input_set_key(NFSU2_DIK_UP, true);
        nfsu2_input_set_controller_key(NFSU2_DIK_LEFT, true);
        assert(nfsu2_input_active_count() == 2);
        nfsu2_input_set_enabled(false);
        assert(!nfsu2_input_is_enabled() && nfsu2_input_active_count() == 0);
        nfsu2_input_set_key(NFSU2_DIK_UP, true);
        nfsu2_input_set_controller_key(NFSU2_DIK_LEFT, true);
        nfsu2_input_set_enabled(true);
        assert(nfsu2_input_is_enabled() && nfsu2_input_active_count() == 0);
        nfsu2_input_set_key(NFSU2_DIK_UP, true);
        nfsu2_input_set_controller_key(NFSU2_DIK_LEFT, true);
        uint8_t keyboard[256]; nfsu2_input_fill_keyboard(keyboard, sizeof(keyboard));
        assert(keyboard[NFSU2_DIK_UP] == 0x80 && keyboard[NFSU2_DIK_LEFT] == 0x80);
        nfsu2_input_clear_all();
    }
    puts("PASS: test_background_foreground_both_sources (50 cycles)");
}

static _Atomic bool rumble_writer_done;
static void *rumble_writer(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 100000; ++i) {
        bool high = (i & 1u) != 0;
        nfsu2_input_trigger_impact(high ? 0.8f : 0.3f,
                                  high ? 0.9f : 0.2f, high ? 200u : 100u);
        nfsu2_input_report_rumble(high ? 0.8f : 0.3f, high ? 0.8f : 0.3f);
    }
    atomic_store_explicit(&rumble_writer_done, true, memory_order_release);
    return NULL;
}

static void test_rumble_concurrent_coherent_messages(void) {
    nfsu2_input_clear_rumble();
    atomic_store(&rumble_writer_done, false);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, rumble_writer, NULL) == 0);
    uint32_t last_id = 0;
    unsigned impacts = 0;
    do {
        NFSU2RumbleSnapshot s;
        nfsu2_input_poll_rumble(&s);
        if (s.impact_intensity > 0) {
            bool high = s.impact_intensity > 0.5f;
            assert(fabsf(s.impact_intensity - (high ? 0.8f : 0.3f)) < 0.001f);
            assert(fabsf(s.impact_sharpness - (high ? 0.9f : 0.2f)) < 0.001f);
            assert(s.impact_duration_ms == (high ? 200u : 100u));
            assert(s.impact_id != last_id);
            last_id = s.impact_id;
            ++impacts;
        }
        if (s.continuous_intensity > 0)
            assert(s.continuous_intensity == s.continuous_sharpness);
    } while (!atomic_load_explicit(&rumble_writer_done, memory_order_acquire));
    assert(pthread_join(thread, NULL) == 0);
    assert(impacts > 0);
    nfsu2_input_clear_rumble();
    nfsu2_input_report_rumble(NAN, INFINITY);
    nfsu2_input_trigger_impact(INFINITY, NAN, 100u);
    NFSU2RumbleSnapshot s;
    assert(!nfsu2_input_poll_rumble(&s));
    puts("PASS: test_rumble_concurrent_coherent_messages (100000 reports + nonfinite input)");
}

int main(void) {
    printf("=== Running NFSU2InputBridge Tests ===\n");
    test_initial_and_clear();
    test_key_set_and_get();
    test_steering_analog();
    test_pedals_analog();
    test_metal_fill_keyboard_compat();
    test_metal_available();
    test_cursor_bridge();
    test_cursor_snapshot_bridge();
    test_metal_fill_mouse_bridge();
    test_rumble_initial_and_null_safety();
    test_rumble_continuous_and_ttl();
    test_rumble_impact_trigger_and_consumption();
    test_rumble_impact_coalescing();
    test_rumble_clear_and_disabled();
    test_rumble_concurrent_coherent_messages();
    test_background_foreground_both_sources();
    printf("=== All NFSU2InputBridge Tests PASSED ===\n");
    return 0;
}
