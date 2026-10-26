/*
 * NFSU2 iOS Port - DirectInput Klavye ve Kontrolcü Köprüsü Uygulaması
 *
 * C11 atomik bellek modeli ile kilitsiz (lock-free), sıfır gecikmeli (zero-latency)
 * tuş dizisi yönetimi sağlar. DirectInput (cb=256) GetDeviceState çağrılarını
 * ve SPEED2.EXE içindeki 0x00870990 statik klavye tamponunu doğrudan besler.
 */

#include "NFSU2InputBridge.h"
#include "guest_cursor.h"
#include "guest_memory.h"
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <math.h>

#if defined(NFS_FULL_RUNTIME) && NFS_FULL_RUNTIME
/* Match DirectInput's GetTickCount-style clock source in runtime/com.c. */
extern uint64_t host_us(void);
#endif

/*
 * DirectInput 8 klavye durumu için 256 baytlık atomik tablo.
 * Her eleman 1 (basılı) veya 0 (serbest) tutar.
 * DirectInput standart GetDeviceState ve GetDeviceData çağrılarında
 * basılı tuşlar için en yüksek bit olan 0x80 beklenir.
 */
static _Atomic uint8_t g_touch_keys[256];
static _Atomic uint8_t g_controller_keys[256];
static _Atomic bool g_input_enabled = ATOMIC_VAR_INIT(true);

#define INPUT_EVENT_CAPACITY 512u
#define INPUT_EVENT_MASK (INPUT_EVENT_CAPACITY - 1u)
typedef struct {
    _Atomic uint8_t dik_code;
    _Atomic uint8_t is_pressed;
    _Atomic uint32_t timestamp_ms;
    _Atomic uint32_t sequence;
    _Atomic uint32_t generation;
} InputKeyEvent;

/* Swift/UI is the single producer; the guest runtime is the single consumer. */
static InputKeyEvent g_input_events[INPUT_EVENT_CAPACITY];
static _Atomic uint32_t g_input_event_head;
static _Atomic uint32_t g_input_event_tail;
static _Atomic uint32_t g_input_event_generation = ATOMIC_VAR_INIT(1u);
static _Atomic uint32_t g_input_event_write_generation = ATOMIC_VAR_INIT(1u);
static _Atomic uint32_t g_input_event_sequence;
static _Atomic uint32_t g_input_event_reset_sequence_base;
static _Atomic bool g_input_event_overflow;

/*
 * Titreşim (Rumble) Durum Değişkenleri
 * Each message is one atomic word: a consumer cannot steal the next impact's
 * intensity while reading the previous impact's ID, sharpness or duration.
 * Continuous: 14-bit intensity, 14-bit sharpness, 4-bit poll TTL.
 * Impact: 14-bit intensity, 14-bit sharpness, 9-bit duration, 27-bit sequence.
 */
static _Atomic uint32_t g_rumble_continuous;
static _Atomic uint64_t g_rumble_impact;
static _Atomic uint32_t g_rumble_impact_id;
static uint32_t g_rumble_delivered_id; /* single Swift/main-thread consumer */
static _Atomic uint32_t g_rumble_hook_calls, g_rumble_local_samples;
static _Atomic uint32_t g_rumble_gated_samples, g_rumble_last_gate;
static _Atomic uint32_t g_rumble_continuous_reports, g_rumble_impact_reports;

static bool input_enabled(void) {
    return atomic_load_explicit(&g_input_enabled, memory_order_acquire);
}

static bool input_key_state(uint8_t dik_code) {
    return atomic_load_explicit(&g_touch_keys[dik_code], memory_order_acquire) != 0 ||
           atomic_load_explicit(&g_controller_keys[dik_code], memory_order_acquire) != 0;
}

static uint32_t input_timestamp_ms(void) {
#if defined(NFS_FULL_RUNTIME) && NFS_FULL_RUNTIME
    return (uint32_t)(host_us() / 1000ull);
#else
    struct timespec now;
    if (timespec_get(&now, TIME_UTC) != TIME_UTC) return 0;
    return (uint32_t)((uint64_t)now.tv_sec * 1000u +
                      (uint64_t)now.tv_nsec / 1000000u);
#endif
}

static void input_queue_edge(uint8_t dik_code, bool is_pressed) {
    uint32_t head = atomic_load_explicit(&g_input_event_head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&g_input_event_tail, memory_order_acquire);
    if ((uint32_t)(head - tail) >= INPUT_EVENT_CAPACITY) {
        if (!atomic_load_explicit(&g_input_event_overflow, memory_order_relaxed)) {
            uint32_t reset_sequence_base = atomic_fetch_add_explicit(
                &g_input_event_sequence, INPUT_EVENT_CAPACITY,
                memory_order_acq_rel) + 1u;
            atomic_store_explicit(&g_input_event_reset_sequence_base,
                                  reset_sequence_base, memory_order_release);
            atomic_store_explicit(&g_input_event_overflow, true,
                                  memory_order_release);
        }
        return;
    }

    InputKeyEvent *event = &g_input_events[head & INPUT_EVENT_MASK];
    atomic_store_explicit(&event->dik_code, dik_code, memory_order_relaxed);
    atomic_store_explicit(&event->is_pressed, is_pressed ? 1u : 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&event->timestamp_ms, input_timestamp_ms(),
                          memory_order_relaxed);
    atomic_store_explicit(&event->sequence,
                          atomic_fetch_add_explicit(&g_input_event_sequence, 1u,
                                                    memory_order_relaxed) + 1u,
                          memory_order_relaxed);
    atomic_store_explicit(&event->generation,
                          atomic_load_explicit(&g_input_event_write_generation,
                                               memory_order_acquire),
                          memory_order_relaxed);
    atomic_store_explicit(&g_input_event_head, head + 1u, memory_order_release);
}

static void recompute_key_locked(uint8_t dik_code, bool was_pressed) {
    bool now_pressed = input_key_state(dik_code);
    if (input_enabled() && was_pressed != now_pressed)
        input_queue_edge(dik_code, now_pressed);
}

static void input_set_source(_Atomic uint8_t *source, uint8_t dik_code,
                             uint8_t v) {
    bool was_pressed = input_key_state(dik_code);
    atomic_store_explicit(&source[dik_code], v, memory_order_release);
    recompute_key_locked(dik_code, was_pressed);
}

void nfsu2_input_set_key(uint8_t dik_code, bool is_pressed) {
    if (!input_enabled()) return;
    uint8_t v = is_pressed ? 1u : 0u;
    if (atomic_load_explicit(&g_touch_keys[dik_code], memory_order_acquire) == v)
        return;
    input_set_source(g_touch_keys, dik_code, v);
}

void nfsu2_input_set_controller_key(uint8_t dik_code, bool is_pressed) {
    if (!input_enabled()) return;
    uint8_t v = is_pressed ? 1u : 0u;
    if (atomic_load_explicit(&g_controller_keys[dik_code], memory_order_acquire) == v)
        return;
    input_set_source(g_controller_keys, dik_code, v);
}

void nfsu2_input_clear_controller(void) {
    for (uint32_t i = 0; i < 256; i++) {
        bool was_pressed = input_key_state((uint8_t)i);
        atomic_store_explicit(&g_controller_keys[i], 0, memory_order_release);
        bool now_pressed = input_key_state((uint8_t)i);
        if (input_enabled() && was_pressed && !now_pressed)
            input_queue_edge((uint8_t)i, false);
    }
    nfsu2_input_clear_rumble();
}

void nfsu2_input_set_enabled(bool enabled) {
    if (enabled) {
        atomic_store_explicit(&g_input_enabled, true, memory_order_release);
        return;
    }

    atomic_store_explicit(&g_input_enabled, false, memory_order_release);
    nfsu2_input_clear_all();
    nfsu2_input_clear_rumble();
    guest_cursor_set_available(false);
}

void nfsu2_input_clear_all(void) {
    uint32_t reset_sequence_base = atomic_fetch_add_explicit(
        &g_input_event_sequence, INPUT_EVENT_CAPACITY, memory_order_acq_rel) + 1u;
    atomic_store_explicit(&g_input_event_reset_sequence_base,
                          reset_sequence_base, memory_order_release);
    uint32_t write_generation = atomic_fetch_add_explicit(
        &g_input_event_write_generation, 1u, memory_order_acq_rel) + 1u;
    for (int i = 0; i < 256; i++) {
        atomic_store_explicit(&g_touch_keys[i], 0, memory_order_relaxed);
        atomic_store_explicit(&g_controller_keys[i], 0, memory_order_relaxed);
    }
    uint32_t head = atomic_load_explicit(&g_input_event_head, memory_order_acquire);
    atomic_store_explicit(&g_input_event_tail, head, memory_order_release);
    atomic_store_explicit(&g_input_event_overflow, false, memory_order_release);
    atomic_store_explicit(&g_input_event_generation, write_generation,
                          memory_order_release);
    nfsu2_input_clear_rumble();
}

bool nfsu2_input_get_key(uint8_t dik_code) {
    return input_enabled() && input_key_state(dik_code);
}

uint32_t nfsu2_input_active_count(void) {
    if (!input_enabled()) return 0;
    uint32_t count = 0;
    for (int i = 0; i < 256; i++) {
        if (input_key_state((uint8_t)i)) {
            count++;
        }
    }
    return count;
}

void nfsu2_input_fill_keyboard(uint8_t *dst, uint32_t count) {
    if (!dst) return;
    if (count > 256) count = 256;
    bool enabled = input_enabled();
    for (uint32_t i = 0; i < count; i++) {
        dst[i] = enabled && input_key_state((uint8_t)i) ? 0x80 : 0x00;
    }
}

uint32_t nfsu2_input_event_snapshot(uint32_t *cursor) {
    uint32_t tail = atomic_load_explicit(&g_input_event_tail, memory_order_acquire);
    uint32_t head = atomic_load_explicit(&g_input_event_head, memory_order_acquire);
    uint32_t count = head - tail;
    if (count > INPUT_EVENT_CAPACITY) count = INPUT_EVENT_CAPACITY;
    if (cursor) *cursor = tail;
    return input_enabled() ? count : 0;
}

uint32_t nfsu2_input_event_generation(void) {
    return atomic_load_explicit(&g_input_event_generation, memory_order_acquire);
}

uint32_t nfsu2_input_event_reset_sequence_base(void) {
    return atomic_load_explicit(&g_input_event_reset_sequence_base,
                                memory_order_acquire);
}

bool nfsu2_input_event_at(uint32_t cursor, uint32_t index,
                          uint8_t *dik_code, bool *is_pressed,
                          uint32_t *timestamp_ms, uint32_t *sequence) {
    if (!input_enabled()) return false;
    if (atomic_load_explicit(&g_input_event_tail, memory_order_acquire) != cursor)
        return false;
    uint32_t head = atomic_load_explicit(&g_input_event_head, memory_order_acquire);
    if ((uint32_t)(head - cursor) > INPUT_EVENT_CAPACITY || index >= head - cursor)
        return false;

    const InputKeyEvent *event = &g_input_events[(cursor + index) & INPUT_EVENT_MASK];
    uint32_t generation = atomic_load_explicit(&g_input_event_generation,
                                                memory_order_acquire);
    uint32_t event_generation = atomic_load_explicit(&event->generation,
                                                     memory_order_relaxed);
    uint8_t key = atomic_load_explicit(&event->dik_code, memory_order_relaxed);
    uint8_t pressed = atomic_load_explicit(&event->is_pressed, memory_order_relaxed);
    uint32_t timestamp = atomic_load_explicit(&event->timestamp_ms,
                                              memory_order_relaxed);
    uint32_t serial = atomic_load_explicit(&event->sequence, memory_order_relaxed);
    if (event_generation != generation || !input_enabled() ||
        generation != atomic_load_explicit(&g_input_event_generation,
                                            memory_order_acquire) ||
        atomic_load_explicit(&g_input_event_tail, memory_order_acquire) != cursor)
        return false;
    if (dik_code) *dik_code = key;
    if (is_pressed) *is_pressed = pressed != 0;
    if (timestamp_ms) *timestamp_ms = timestamp;
    if (sequence) *sequence = serial;
    return true;
}

void nfsu2_input_consume_events(uint32_t cursor, uint32_t count) {
    if (!count) return;
    uint32_t head = atomic_load_explicit(&g_input_event_head, memory_order_acquire);
    uint32_t available = head - cursor;
    if (available > INPUT_EVENT_CAPACITY) return;
    if (count > available) count = available;
    uint32_t expected = cursor;
    (void)atomic_compare_exchange_strong_explicit(&g_input_event_tail, &expected,
                                                  cursor + count,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire);
}

bool nfsu2_input_take_event_overflow(void) {
    return atomic_exchange_explicit(&g_input_event_overflow, false,
                                   memory_order_acq_rel);
}

bool nfsu2_input_event_overflowed(void) {
    return atomic_load_explicit(&g_input_event_overflow, memory_order_acquire);
}

void nfsu2_input_sync_guest_memory(void) {
    /* Documented no-op: SPEED2.EXE reads DirectInput keyboard states through
     * IDirectInputDevice8::GetDeviceState, which calls metal_fill_keyboard. */
}

void nfsu2_input_set_steering(float analog_x) {
    /* Hassas ölü bölge (deadzone) eşiği */
    const float deadzone = 0.08f;
    if (analog_x < -deadzone) {
        /* Sol yön aktif: hem DIK_LEFT hem de ikincil tuş DIK_A */
        nfsu2_input_set_key(NFSU2_DIK_LEFT, true);
        nfsu2_input_set_key(NFSU2_DIK_A, true);
        nfsu2_input_set_key(NFSU2_DIK_RIGHT, false);
        nfsu2_input_set_key(NFSU2_DIK_D, false);
    } else if (analog_x > deadzone) {
        /* Sağ yön aktif: hem DIK_RIGHT hem de ikincil tuş DIK_D */
        nfsu2_input_set_key(NFSU2_DIK_RIGHT, true);
        nfsu2_input_set_key(NFSU2_DIK_D, true);
        nfsu2_input_set_key(NFSU2_DIK_LEFT, false);
        nfsu2_input_set_key(NFSU2_DIK_A, false);
    } else {
        /* Ölü bölge içinde (merkez): direksiyon tuşlarını serbest bırak */
        nfsu2_input_set_key(NFSU2_DIK_LEFT, false);
        nfsu2_input_set_key(NFSU2_DIK_A, false);
        nfsu2_input_set_key(NFSU2_DIK_RIGHT, false);
        nfsu2_input_set_key(NFSU2_DIK_D, false);
    }
}

void nfsu2_input_set_throttle(float analog_val) {
    const float threshold = 0.08f;
    bool pressed = (analog_val > threshold);
    nfsu2_input_set_key(NFSU2_DIK_UP, pressed);
    nfsu2_input_set_key(NFSU2_DIK_W, pressed);
}

void nfsu2_input_set_brake(float analog_val) {
    const float threshold = 0.08f;
    bool pressed = (analog_val > threshold);
    nfsu2_input_set_key(NFSU2_DIK_DOWN, pressed);
    nfsu2_input_set_key(NFSU2_DIK_S, pressed);
}

/*
 * runtime/com.c IDirectInputDevice8::GetDeviceState ve GetDeviceData çağrıları
 * metal_fill_keyboard() fonksiyonunu çağırır. Bu fonksiyon tamponu doğrudan doldurur.
 */
void metal_fill_keyboard(unsigned char *dst, unsigned int n) {
    nfsu2_input_fill_keyboard((uint8_t *)dst, (uint32_t)n);
}

/*
 * runtime/com.c uyumluluğu: DirectInput hook'larının metal_fill_keyboard()
 * çağrısını yapabilmesi için metal_available() gereklidir.
 */
#if !defined(NFS_FULL_RUNTIME)
__attribute__((weak)) int metal_available(void) {
    return 1;
}
#endif

void nfsu2_input_set_cursor(float host_x, float host_y, bool is_down) {
    if (!input_enabled()) return;
    guest_cursor_set_host_pos(host_x, host_y, is_down);
}

void nfsu2_input_set_cursor_available(bool available) {
    if (available && !input_enabled()) return;
    guest_cursor_set_available(available);
}

bool nfsu2_input_get_cursor(int *x, int *y) {
    if (!input_enabled()) {
        if (x) *x = 0;
        if (y) *y = 0;
        return false;
    }
    GuestCursorSnapshot snapshot = guest_cursor_get_snapshot();
    if (x) *x = snapshot.coords.x;
    if (y) *y = snapshot.coords.y;
    return (snapshot.flags & GUEST_CURSOR_FLAG_AVAILABLE) != 0;
}

bool nfsu2_input_get_cursor_snapshot(int32_t *x, int32_t *y, uint32_t *flags) {
    if (!input_enabled()) {
        if (x) *x = 0;
        if (y) *y = 0;
        if (flags) *flags = 0;
        return false;
    }
    GuestCursorSnapshot snapshot = guest_cursor_get_snapshot();
    if (x) *x = snapshot.coords.x;
    if (y) *y = snapshot.coords.y;
    if (flags) *flags = snapshot.flags;
    return snapshot.position_valid &&
           (snapshot.flags & GUEST_CURSOR_FLAG_AVAILABLE) != 0;
}

void nfsu2_input_set_viewport(uint32_t viewport_w, uint32_t viewport_h,
                               uint32_t game_w, uint32_t game_h) {
    guest_cursor_set_viewport(viewport_w, viewport_h, game_w, game_h, GUEST_CURSOR_SCALE_FIT);
}

/*
 * Titreşim (Rumble / Haptics) API Fonksiyonları
 */

static uint32_t rumble_fixed(float value) {
    if (!isfinite(value) || value <= 0.0f) return 0;
    if (value >= 1.0f) return 10000u;
    return (uint32_t)(value * 10000.0f + 0.5f);
}

void nfsu2_input_note_rumble_hook(uint32_t gate) {
    atomic_fetch_add_explicit(&g_rumble_hook_calls, 1u, memory_order_relaxed);
    if (gate == 3u || gate == 4u)
        atomic_fetch_add_explicit(&g_rumble_local_samples, 1u, memory_order_relaxed);
    if (gate == 3u)
        atomic_fetch_add_explicit(&g_rumble_gated_samples, 1u, memory_order_relaxed);
    /* Other cars must not overwrite the local player's diagnostic gate. */
    if (gate != 2u)
        atomic_store_explicit(&g_rumble_last_gate, gate, memory_order_relaxed);
}

void nfsu2_input_get_rumble_diagnostics(NFSU2RumbleDiagnostics *d) {
    if (!d) return;
    d->hook_calls = atomic_load_explicit(&g_rumble_hook_calls, memory_order_relaxed);
    d->local_samples = atomic_load_explicit(&g_rumble_local_samples, memory_order_relaxed);
    d->gated_samples = atomic_load_explicit(&g_rumble_gated_samples, memory_order_relaxed);
    d->continuous_reports = atomic_load_explicit(&g_rumble_continuous_reports, memory_order_relaxed);
    d->impact_reports = atomic_load_explicit(&g_rumble_impact_reports, memory_order_relaxed);
    d->last_gate = atomic_load_explicit(&g_rumble_last_gate, memory_order_relaxed);
}

void nfsu2_input_clear_rumble(void) {
    atomic_store_explicit(&g_rumble_continuous, 0, memory_order_release);
    atomic_store_explicit(&g_rumble_impact, 0, memory_order_release);
}

void nfsu2_input_report_rumble(float intensity, float sharpness) {
    uint32_t i = rumble_fixed(intensity), s = rumble_fixed(sharpness);
    uint32_t message = input_enabled() && i > 10u
        ? i | (s << 14) | (8u << 28) : 0u;
    atomic_store_explicit(&g_rumble_continuous, message, memory_order_release);
    if (message)
        atomic_fetch_add_explicit(&g_rumble_continuous_reports, 1u, memory_order_relaxed);
    if (!input_enabled())
        atomic_store_explicit(&g_rumble_continuous, 0, memory_order_release);
}

void nfsu2_input_trigger_impact(float intensity, float sharpness, uint32_t duration_ms) {
    if (!input_enabled()) return;
    uint32_t i = rumble_fixed(intensity), s = rumble_fixed(sharpness);
    if (i <= 100u) return;
    if (duration_ms < 60u) duration_ms = 60u;
    if (duration_ms > 280u) duration_ms = 280u;
    uint32_t id = (atomic_fetch_add_explicit(&g_rumble_impact_id, 1u,
                                            memory_order_relaxed) + 1u) & 0x7ffffffu;
    uint64_t message = (uint64_t)i | ((uint64_t)s << 14) |
        ((uint64_t)duration_ms << 28) | ((uint64_t)id << 37);
    uint64_t previous = atomic_load_explicit(&g_rumble_impact, memory_order_acquire);
    do {
        if ((previous & 0x3fffu) >= i) return;
    } while (!atomic_compare_exchange_weak_explicit(&g_rumble_impact, &previous,
                   message, memory_order_acq_rel, memory_order_acquire));
    atomic_fetch_add_explicit(&g_rumble_impact_reports, 1u, memory_order_relaxed);
    if (!input_enabled())
        atomic_store_explicit(&g_rumble_impact, 0, memory_order_release);
}

bool nfsu2_input_poll_rumble(NFSU2RumbleSnapshot *snapshot) {
    if (!snapshot) return false;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->impact_id = g_rumble_delivered_id;
    if (!input_enabled()) {
        nfsu2_input_clear_rumble();
        return false;
    }
    uint32_t continuous = atomic_load_explicit(&g_rumble_continuous, memory_order_acquire);
    while (continuous >> 28) {
        uint32_t next = continuous - (1u << 28);
        if (atomic_compare_exchange_weak_explicit(&g_rumble_continuous, &continuous,
                next, memory_order_acq_rel, memory_order_acquire)) {
            snapshot->continuous_intensity = (float)(continuous & 0x3fffu) / 10000.0f;
            snapshot->continuous_sharpness = (float)((continuous >> 14) & 0x3fffu) / 10000.0f;
            break;
        }
    }
    uint64_t impact = atomic_exchange_explicit(&g_rumble_impact, 0, memory_order_acq_rel);
    if (impact) {
        snapshot->impact_intensity = (float)(impact & 0x3fffu) / 10000.0f;
        snapshot->impact_sharpness = (float)((impact >> 14) & 0x3fffu) / 10000.0f;
        snapshot->impact_duration_ms = (uint32_t)((impact >> 28) & 0x1ffu);
        snapshot->impact_id = g_rumble_delivered_id = (uint32_t)(impact >> 37);
    }
    if (!input_enabled()) {
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->impact_id = g_rumble_delivered_id;
        return false;
    }
    return snapshot->continuous_intensity > 0.001f || snapshot->impact_intensity > 0.001f;
}

void nfsu2_input_reset(void) {
    nfsu2_input_clear_all();
    nfsu2_input_clear_rumble();
}
