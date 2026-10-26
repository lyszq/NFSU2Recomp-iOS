/*
 * NFSU2 iOS Port - DirectInput (DIK_*) Klavye ve Kontrolcü Köprüsü
 *
 * DirectInput 8 klavye tarama kodları (DIK_*) tamponunu (g_keys[256]) yönetir.
 * Swift dokunmatik kontrolleri ve GameController.framework (GCController)
 * girdilerini doğrudan C çalışma zamanına (runtime/com.c) gecikmesiz aktarır.
 */

#ifndef NFSU2_INPUT_BRIDGE_H
#define NFSU2_INPUT_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DirectInput 8 Tarama Kodları (DIK_*)
 * Need for Speed: Underground 2 varsayılan kontrol haritasıyla birebir eşleşir.
 */
#define NFSU2_DIK_ESCAPE          0x01  /* Menü / Duraklat / Geri */
#define NFSU2_DIK_1               0x02
#define NFSU2_DIK_2               0x03
#define NFSU2_DIK_3               0x04
#define NFSU2_DIK_BACK            0x0E  /* Geri Al / Backspace */
#define NFSU2_DIK_TAB             0x0F  /* Harita / Skor */
#define NFSU2_DIK_Q               0x10  /* Vites Küçült (Manuel) */
#define NFSU2_DIK_W               0x11  /* İkincil Gaz */
#define NFSU2_DIK_E               0x12  /* Vites Yükselt (Manuel) */
#define NFSU2_DIK_R               0x13  /* Araba Sıfırla (Reset Car) */
#define NFSU2_DIK_RETURN          0x1C  /* Seç / Menü Enter */
#define NFSU2_DIK_LCONTROL        0x1D
#define NFSU2_DIK_A               0x1E  /* İkincil Sol */
#define NFSU2_DIK_S               0x1F  /* İkincil Fren / Geri */
#define NFSU2_DIK_D               0x20  /* İkincil Sağ */
#define NFSU2_DIK_L               0x26  /* SMS / Mesajlar */
#define NFSU2_DIK_LSHIFT          0x2A  /* NOS / Nitro */
#define NFSU2_DIK_C               0x2E  /* Kamera Görünümü Değiştir */
#define NFSU2_DIK_B               0x30  /* Arkaya Bak (Look Back) */
#define NFSU2_DIK_N               0x31  /* İkincil NOS */
#define NFSU2_DIK_M               0x32  /* Harita / GPS */
#define NFSU2_DIK_SPACE           0x39  /* El Freni (Handbrake) */
#define NFSU2_DIK_UP              0xC8  /* Gaz / Hızlanma (Throttle) */
#define NFSU2_DIK_LEFT            0xCB  /* Direksiyon Sol (Steer Left) */
#define NFSU2_DIK_RIGHT           0xCD  /* Direksiyon Sağ (Steer Right) */
#define NFSU2_DIK_DOWN            0xD0  /* Fren / Geri Vites (Brake/Reverse) */

/*
 * Swift -> C Köprü Fonksiyonları (Sıfır Gecikme, Atomik Bellek Erişimi)
 */

/* Dokunmatik/genel kaynak için DIK_* durumunu ayarlar. */
void nfsu2_input_set_key(uint8_t dik_code, bool is_pressed);

/* Kontrolcü kaynağını dokunmatikten ayrı tutar; iki kaynak getter'da OR edilir. */
void nfsu2_input_set_controller_key(uint8_t dik_code, bool is_pressed);
void nfsu2_input_clear_controller(void);

/* Uygulama menüsü/background sırasında bütün oyun girişini kapatır. */
void nfsu2_input_set_enabled(bool enabled);
bool nfsu2_input_is_enabled(void);

/* Tüm tuş tamponunu (g_keys) sıfırlar (arka plana atılınca, kopmada veya duraklatmada çağrılır) */
void nfsu2_input_clear_all(void);

/* Belirli bir tuşun durumunu okur */
bool nfsu2_input_get_key(uint8_t dik_code);

/* Şu anda basılı olan aktif tuş sayısını döndürür */
uint32_t nfsu2_input_active_count(void);

/* DirectInput 256 baytlık klavye tablosunu doldurur (0x80 = basılı, 0x00 = basılı değil) */
void nfsu2_input_fill_keyboard(uint8_t *dst, uint32_t count);

/*
 * DirectInput buffered input için birleşik kaynakların kenar kuyruğu.
 * index, mevcut kuyruğun başından ölçülür; false stale/olmayan kaydı belirtir.
 */
uint32_t nfsu2_input_event_snapshot(uint32_t *cursor);
uint32_t nfsu2_input_event_generation(void);
uint32_t nfsu2_input_event_reset_sequence_base(void);
bool nfsu2_input_event_at(uint32_t cursor, uint32_t index,
                          uint8_t *dik_code, bool *is_pressed,
                          uint32_t *timestamp_ms, uint32_t *sequence);
void nfsu2_input_consume_events(uint32_t cursor, uint32_t count);
bool nfsu2_input_event_overflowed(void);
bool nfsu2_input_take_event_overflow(void);

/*
 * Konuk belleğe (guest memory) doğrudan eşitleme.
 * Eğer SPEED2.EXE belleğe yüklenmiş ve 0x00870990 adresi ayrılmış (committed) ise,
 * tuş durumunu doğrudan 0x00870990 adresindeki 256 baytlık diziye yansıtır.
 */
void nfsu2_input_sync_guest_memory(void);

/*
 * Analog giriş yardımcıları: Analog tetik, direksiyon kaydırıcı (steer slider)
 * ve çubuk girdilerini dijital DIK_* kodlarına çevirir.
 */
void nfsu2_input_set_steering(float analog_x);  /* -1.0 (tam sol) .. +1.0 (tam sağ) */
void nfsu2_input_set_throttle(float analog_val); /* 0.0 .. 1.0 */
void nfsu2_input_set_brake(float analog_val);    /* 0.0 .. 1.0 */

/*
 * Fare / İmleç ve Dokunmatik Giriş Köprüsü (SwiftUI / UIKit -> C Çalışma Zamanı)
 */
enum {
    NFSU2_INPUT_CURSOR_FLAG_AVAILABLE = 1u << 0,
    NFSU2_INPUT_CURSOR_FLAG_DOWN = 1u << 2,
    NFSU2_INPUT_CURSOR_FLAG_SYNTHETIC = 1u << 3
};
void nfsu2_input_set_cursor(float host_x, float host_y, bool is_down);
void nfsu2_input_set_cursor_available(bool available);
bool nfsu2_input_get_cursor(int *x, int *y);
/* Returns one coherent snapshot. False means no valid position with AVAILABLE set. */
bool nfsu2_input_get_cursor_snapshot(int32_t *x, int32_t *y, uint32_t *flags);
void nfsu2_input_set_viewport(uint32_t viewport_w, uint32_t viewport_h,
                               uint32_t game_w, uint32_t game_h);

/*
 * Oyun İçi Titreşim (Rumble / Haptics) Köprüsü
 * Oyun içi fizik/çarpışma kancasından Swift ControllerHaptics motoruna
 * gerçek zamanlı titreşim verisi aktarır.
 */
typedef struct {
    float continuous_intensity; /* 0.0f .. 1.0f */
    float continuous_sharpness; /* 0.0f .. 1.0f */
    float impact_intensity;     /* 0.0f .. 1.0f */
    float impact_sharpness;     /* 0.0f .. 1.0f */
    uint32_t impact_duration_ms;/* 60 .. 280 ms */
    uint32_t impact_id;         /* sequence modulo 2^27; belongs to this snapshot */
} NFSU2RumbleSnapshot;

void nfsu2_input_report_rumble(float continuous_intensity, float continuous_sharpness);
void nfsu2_input_trigger_impact(float intensity, float sharpness, uint32_t duration_ms);
void nfsu2_input_clear_rumble(void);
bool nfsu2_input_poll_rumble(NFSU2RumbleSnapshot *snapshot);
/* Diagnostic counters are process-lifetime, independent of input clearing. */
typedef struct {
    uint32_t hook_calls, local_samples, gated_samples;
    uint32_t continuous_reports, impact_reports, last_gate;
} NFSU2RumbleDiagnostics;
/* 1: invalid vehicle, 2: AI, 3: local but inactive, 4: local active. */
void nfsu2_input_note_rumble_hook(uint32_t gate);
void nfsu2_input_get_rumble_diagnostics(NFSU2RumbleDiagnostics *diagnostics);
void nfsu2_input_reset(void);

/*
 * runtime/com.c ile geriye dönük uyumluluk:
 * com.c metal_fill_keyboard() fonksiyonunu çağırır ve metal_available() sorgular.
 */
void metal_fill_keyboard(unsigned char *dst, unsigned int n);
int  metal_available(void);

#ifdef __cplusplus
}
#endif

#endif /* NFSU2_INPUT_BRIDGE_H */
