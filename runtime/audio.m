/* NFSU2 iOS Port - CoreAudio / RemoteIO / DirectSound Audio Layer
 *
 * EA Trax (interaktif muzik akisi), SFX (ses efektleri), ve motor sesleri
 * icin iOS CoreAudio cikis yolu. Uretim karistiricisi host PCM aynasini
 * g_slock ile koruyarak okur; ring-buffer yardimcilari
 * su anda uretim render yoluna bagli degildir.
 *
 * iOS AVAudioSession ve RemoteIO AudioUnit yasam dongusu:
 * - Kesintiler (AVAudioSessionInterruptionNotification): Arama / Alarm sirasinda
 *   ses unitesi guvenli sekilde durdurulur, sonrasinda otomatik surdurulur.
 * - Rota degisiklikleri (AVAudioSessionRouteChangeNotification): AirPods veya
 *   kulaklik takilip cikarildiginda donanim ornekleme hizi (44.1 <-> 48 kHz)
 *   ve tampon boyutu calisma aninda yeniden yapilandirilir.
 * - Medya sifirlama (AVAudioSessionMediaServicesWereResetNotification): audiod
 *   cokerse AudioUnit bastan olusturulur.
 * - au_cb -> mix_into -> mix_chunk yolu g_slock alir; uretim callback'i
 *   kilit-serbest degildir ve tanilama aciksa dosyaya yazabilir.
 */

#ifndef NFSU2_AUDIO_CORE_TEST
#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AudioUnit.h>
#include <TargetConditionals.h>

#if TARGET_OS_IPHONE
#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>
#endif
#else
/* Compile the exact production mixer/API as C for deterministic host tests. */
#define TARGET_OS_IPHONE 0
#endif

#include "audio.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <math.h>
#include <time.h>
#include <stdatomic.h>

extern uint8_t *g_mem;
uint64_t host_us(void);          /* iOS: continuous monotonic; desktop: legacy virtual clock */

/* --- Donanim ve Durum Degiskenleri --- */
static atomic_uint_fast32_t g_hw_rate        = AUDIO_DEFAULT_RATE; /* 44100 veya 48000 Hz */
static atomic_bool          g_audio_suspended   = false;
static atomic_bool          g_audio_reconfiguring = false;
static atomic_bool          g_audio_on          = false;

/* Legacy desktop policy: oyun ses verisini kendi sanal zamaninda uretir; karistirici ise gercek
 * zamanda tuketiyordu. Emulator 60 fps altina dusunce sesin kesilmesini
 * onlemek icin tuketimi oyunun uretim hizina baglayan servo (PLL). */
static double g_rate_ratio = 1.0;
static int    g_sync_mode  = -1;      /* SNDSYNC: 1 = oyun hizina bagla */

static void update_rate_ratio(void){
#if TARGET_OS_IPHONE || defined(NFSU2_AUDIO_CORE_TEST)
    /* iOS guest time is continuous monotonic time now. Audio output runs at
     * the hardware clock, so do not let a virtual-clock servo pitch-shift it. */
    g_sync_mode = 1;
    g_rate_ratio = 1.0;
#else
    if (g_sync_mode < 0){
        const char *e = getenv("SNDSYNC");
        g_sync_mode = (e && *e) ? (*e != '0') : 1;      /* varsayilan: acik */
    }
    if (!g_sync_mode){ g_rate_ratio = 1.0; return; }
    static uint64_t lv = 0, lr = 0;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t rus = (uint64_t)ts.tv_sec*1000000ull + (uint64_t)ts.tv_nsec/1000ull;
    uint64_t vus = host_us();
    if (lv && rus > lr){
        double dv = (double)(vus - lv), dr = (double)(rus - lr);
        if (dr > 1000.0){
            double k = dv / dr;
            if (k < 0.25) k = 0.25;
            /* TAVAN 1.0: ses GERCEK ZAMANDAN HIZLI calmamali. */
            if (k > 1.00) k = 1.00;
            g_rate_ratio += (k - g_rate_ratio) * 0.10;
        }
    }
    lv = vus; lr = rus;
#endif
}

#define OUT_CH     AUDIO_OUT_CHANNELS
#define NBUF       6
#define FRAMES     4096
#define WRITE_LEAD_MS 50u

#define MAXSND     AUDIO_MAX_BUFFERS
#define AUDIO_PCM_MIRROR_LIMIT_BYTES (64ull * 1024ull * 1024ull)

typedef struct {
    uint32_t data, bytes, rate, chans, bits;
    uint32_t storage_id;      /* ID of the shared guest PCM allocation */
    uint8_t *pcm;              /* Host-owned mirror, shared by duplicates */
    int      used, playing, loop;
    double   cursor;          /* kaynak ornek cinsinden */
    float    vol;             /* 0..1 */
    uint32_t wpos;            /* oyunun yazdigi son baytin bitisi */
    int      wseen;           /* hic yazma gorduk mu */
    uint32_t stream_writes;
    uint32_t diag_write_end_offset;
    uint32_t diag_last_write_bytes;
    int      diag_write_seen;
    uint64_t diag_write_calls, diag_write_bytes;
    uint64_t diag_last_write_us, diag_max_write_gap_us;
    double   trim;            /* hiz duzeltmesi (servo) */
    double   target;          /* kilitlenecek dolgu orani (uyarlanir) */
    double   freq;            /* SetFrequency orani (1.0 = ozgun hiz) */
    uint64_t plays, stops, reissues, wr_bytes, wr_calls, wr_nonzero;
    uint64_t over;            /* yaziciyi gectigimiz geri-cagirma sayisi */
} SndBuf;

static SndBuf g_snd[MAXSND];
static int    g_sndn = 1;                    /* 0 = gecersiz kimlik */
static pthread_mutex_t g_slock = PTHREAD_MUTEX_INITIALIZER;
static void audio_cursors_locked(const SndBuf *b, uint32_t *play, uint32_t *write);
/* Mirrors live for the process: COM buffers survive iOS audio restarts, and
 * this layer has no authoritative final-release hook. */
static uint64_t g_pcm_mirror_bytes = 0;
static uint64_t g_pcm_mirror_reserved_bytes = 0;
static uint64_t g_pcm_publish_calls = 0;
static uint64_t g_pcm_publish_bytes = 0;
static uint64_t g_pcm_publish_max_ns = 0;
static uint64_t g_pcm_rejected_writes = 0;
static int g_pcm_register_failure_logged = 0;

static int                  g_backend = 0;   /* 1 = AudioUnit, 2 = AudioQueue */
static unsigned             g_au_slice = 0;  /* Geri cagirma basina gerceklesen kare */
#ifndef NFSU2_AUDIO_CORE_TEST
static AudioQueueRef        g_q;
static AudioQueueBufferRef  g_qb[NBUF];
static AudioUnit            g_au = NULL;     /* Dusuk gecikmeli RemoteIO */
#endif

static uint64_t g_mixed_frames = 0, g_active_peak = 0;
static uint64_t g_replay = 0, g_reissue = 0, g_starved = 0, g_worst_gap_us = 0;
static uint64_t g_late_callbacks = 0;
static uint64_t g_out_frames = 0, g_out_zero = 0, g_gap_cb = 0;

/* The iOS audio callback must not fall back to a library lock for diagnostics.
 * 64-bit integer atomics are lock-free on the supported arm64 targets. */
_Static_assert(sizeof(uint_fast64_t) == 8, "audio diagnostics require 64-bit counters");
_Static_assert(ATOMIC_LONG_LOCK_FREE == 2 && ATOMIC_LLONG_LOCK_FREE == 2 &&
               ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_BOOL_LOCK_FREE == 2,
               "audio diagnostics require lock-free integer and boolean atomics");
static atomic_uint_fast64_t g_cb_count = 0;
static atomic_uint_fast64_t g_cb_gap_max_us = 0;
static atomic_uint_fast64_t g_cb_duration_max_us = 0;
static atomic_uint_fast64_t g_cb_late_count = 0;
static atomic_uint_fast64_t g_cb_over_budget_count = 0;
static atomic_uint_fast32_t g_cb_max_frames = 0;
static atomic_bool g_cb_clock_reset = true;

/* Tani dosyalari */
static FILE *g_dump = NULL;      /* SNDDUMP=<yol> : ham s16le stereo */
static FILE *g_csv  = NULL;      /* SNDCSV=<yol>  : geri-cagirma basina durum */
static uint64_t g_cb_no = 0;

static void audio_count(uint64_t *counter){
    if (*counter != UINT64_MAX) (*counter)++;
}

static void audio_add(uint64_t *counter, uint64_t amount){
    if (UINT64_MAX - *counter < amount) *counter = UINT64_MAX;
    else *counter += amount;
}

#ifndef NFSU2_AUDIO_CORE_TEST
static void audio_atomic_max_u64(atomic_uint_fast64_t *counter, uint64_t value){
    uint_fast64_t seen = atomic_load_explicit(counter, memory_order_relaxed);
    while (seen < value &&
           !atomic_compare_exchange_weak_explicit(counter, &seen, (uint_fast64_t)value,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {}
}

static void audio_atomic_max_u32(atomic_uint_fast32_t *counter, uint32_t value){
    uint_fast32_t seen = atomic_load_explicit(counter, memory_order_relaxed);
    while (seen < value &&
           !atomic_compare_exchange_weak_explicit(counter, &seen, (uint_fast32_t)value,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {}
}
#endif

/* Monotonic host time for producer and callback diagnostics; never guest/QPC time. */
static uint64_t audio_monotonic_us(void){
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static uint64_t audio_monotonic_ns(void){
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* --- Kilit-Serbest (Lock-Free) SPSC Dairesel Tampon Uygulamasi --- */
#ifndef NFSU2_AUDIO_CORE_TEST
static AudioRingBuffer g_stream_rb;
#endif

bool audio_ringbuf_init(AudioRingBuffer *rb, size_t capacity_frames, uint32_t channels){
    if (!rb || (capacity_frames & (capacity_frames - 1)) != 0) return false;
    rb->channels = (channels > 0) ? channels : OUT_CH;
    rb->capacity_frames = capacity_frames;
    rb->mask = capacity_frames - 1;
    atomic_init(&rb->write_pos, 0);
    atomic_init(&rb->read_pos, 0);
    rb->buffer = (int16_t *)calloc(capacity_frames * rb->channels, sizeof(int16_t));
    return rb->buffer != NULL;
}

void audio_ringbuf_destroy(AudioRingBuffer *rb){
    if (rb && rb->buffer){
        free(rb->buffer);
        rb->buffer = NULL;
    }
}

void audio_ringbuf_reset(AudioRingBuffer *rb){
    if (!rb) return;
    atomic_store_explicit(&rb->write_pos, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->read_pos, 0, memory_order_relaxed);
    if (rb->buffer){
        memset(rb->buffer, 0, rb->capacity_frames * rb->channels * sizeof(int16_t));
    }
}

size_t audio_ringbuf_available_read(const AudioRingBuffer *rb){
    if (!rb) return 0;
    size_t w = atomic_load_explicit(&rb->write_pos, memory_order_acquire);
    size_t r = atomic_load_explicit(&rb->read_pos, memory_order_relaxed);
    return (w >= r) ? (w - r) : 0;
}

size_t audio_ringbuf_available_write(const AudioRingBuffer *rb){
    if (!rb) return 0;
    size_t w = atomic_load_explicit(&rb->write_pos, memory_order_relaxed);
    size_t r = atomic_load_explicit(&rb->read_pos, memory_order_acquire);
    size_t used = (w >= r) ? (w - r) : 0;
    return (used < rb->capacity_frames) ? (rb->capacity_frames - used) : 0;
}

size_t audio_ringbuf_write(AudioRingBuffer *rb, const int16_t *src, size_t frames){
    if (!rb || !src || !frames) return 0;
    size_t avail = audio_ringbuf_available_write(rb);
    if (frames > avail) frames = avail;
    if (frames == 0) return 0;

    size_t w = atomic_load_explicit(&rb->write_pos, memory_order_relaxed);
    size_t idx = w & rb->mask;
    size_t first = rb->capacity_frames - idx;
    if (first > frames) first = frames;
    size_t second = frames - first;

    memcpy(rb->buffer + idx * rb->channels, src, first * rb->channels * sizeof(int16_t));
    if (second > 0){
        memcpy(rb->buffer, src + first * rb->channels, second * rb->channels * sizeof(int16_t));
    }

    atomic_store_explicit(&rb->write_pos, w + frames, memory_order_release);
    return frames;
}

size_t audio_ringbuf_read(AudioRingBuffer *rb, int16_t *dst, size_t frames){
    if (!rb || !dst || !frames) return 0;
    size_t avail = audio_ringbuf_available_read(rb);
    if (frames > avail) frames = avail;
    if (frames == 0) return 0;

    size_t r = atomic_load_explicit(&rb->read_pos, memory_order_relaxed);
    size_t idx = r & rb->mask;
    size_t first = rb->capacity_frames - idx;
    if (first > frames) first = frames;
    size_t second = frames - first;

    memcpy(dst, rb->buffer + idx * rb->channels, first * rb->channels * sizeof(int16_t));
    if (second > 0){
        memcpy(dst + first * rb->channels, rb->buffer, second * rb->channels * sizeof(int16_t));
    }

    atomic_store_explicit(&rb->read_pos, r + frames, memory_order_release);
    return frames;
}

/* --- DirectSound COM Katmani Fonksiyonlari --- */

static void audio_log_register_failure_once(uint32_t bytes){
    int should_log = 0;
    uint64_t mirror_bytes;
    uint64_t reserved_bytes;
    pthread_mutex_lock(&g_slock);
    mirror_bytes = g_pcm_mirror_bytes;
    reserved_bytes = g_pcm_mirror_reserved_bytes;
    if (!g_pcm_register_failure_logged){
        g_pcm_register_failure_logged = 1;
        should_log = 1;
    }
    pthread_mutex_unlock(&g_slock);
    if (should_log){
        fprintf(stderr,
                "  [ses] audio_register failed: PCM mirror budget/allocation/slot limit "
                "(request=%u, mirrors=%llu, reserved=%llu, cap=%llu bytes)\n",
                bytes, (unsigned long long)mirror_bytes,
                (unsigned long long)reserved_bytes,
                (unsigned long long)AUDIO_PCM_MIRROR_LIMIT_BYTES);
    }
}

static void audio_release_mirror_reservation(uint32_t bytes){
    pthread_mutex_lock(&g_slock);
    if (g_pcm_mirror_reserved_bytes >= bytes) g_pcm_mirror_reserved_bytes -= bytes;
    else g_pcm_mirror_reserved_bytes = 0;
    pthread_mutex_unlock(&g_slock);
}

int audio_registration_available(uint32_t bytes){
    pthread_mutex_lock(&g_slock);
    uint64_t used = g_pcm_mirror_bytes + g_pcm_mirror_reserved_bytes;
    int available = bytes && g_sndn < MAXSND && used <= AUDIO_PCM_MIRROR_LIMIT_BYTES &&
                    (uint64_t)bytes <= AUDIO_PCM_MIRROR_LIMIT_BYTES - used;
    pthread_mutex_unlock(&g_slock);
    if (!available) audio_log_register_failure_once(bytes);
    return available;
}

int audio_register(uint32_t data, uint32_t bytes, uint32_t rate,
                   uint32_t chans, uint32_t bits){
    if (!data || !bytes || !g_mem ||
        (uint64_t)data + (uint64_t)bytes > 0x100000000ull){
        audio_log_register_failure_once(bytes);
        return 0;
    }
    if (rate < 4000u || rate > 192000u) rate = AUDIO_DEFAULT_RATE;
    if (chans < 1u || chans > 2u) chans = 1u;
    if (bits != 8u && bits != 16u) bits = 16u;

    /* Reserve the bounded process-lifetime mirror budget under the mixer
     * mutex, then allocate and seed it without holding the callback lock. */
    int reserved = 0;
    pthread_mutex_lock(&g_slock);
    uint64_t used = g_pcm_mirror_bytes + g_pcm_mirror_reserved_bytes;
    if (g_sndn < MAXSND && used <= AUDIO_PCM_MIRROR_LIMIT_BYTES &&
        (uint64_t)bytes <= AUDIO_PCM_MIRROR_LIMIT_BYTES - used){
        g_pcm_mirror_reserved_bytes += bytes;
        reserved = 1;
    }
    pthread_mutex_unlock(&g_slock);
    if (!reserved){
        audio_log_register_failure_once(bytes);
        return 0;
    }

    uint8_t *pcm = (uint8_t *)malloc(bytes);
    if (!pcm){
        audio_release_mirror_reservation(bytes);
        audio_log_register_failure_once(bytes);
        return 0;
    }
    memcpy(pcm, g_mem + data, bytes);

    int id = 0;
    pthread_mutex_lock(&g_slock);
    if (g_pcm_mirror_reserved_bytes >= bytes) g_pcm_mirror_reserved_bytes -= bytes;
    else g_pcm_mirror_reserved_bytes = 0;
    if (g_sndn < MAXSND){
        id = g_sndn++;
        SndBuf *b = &g_snd[id];
        memset(b, 0, sizeof(*b));
        b->data = data; b->bytes = bytes; b->rate = rate;
        b->storage_id = (uint32_t)id;
        b->pcm = pcm;
        b->chans = chans; b->bits = bits;
        b->used = 1; b->playing = 0; b->loop = 0; b->cursor = 0.0; b->vol = 1.f;
        b->wpos = 0; b->wseen = 0; b->stream_writes = 0; b->trim = 1.0; b->target = 0.0; b->over = 0;
        b->diag_write_end_offset = b->diag_last_write_bytes = 0;
        b->diag_write_seen = 0;
        b->diag_write_calls = b->diag_write_bytes = 0;
        b->diag_last_write_us = b->diag_max_write_gap_us = 0;
        b->freq = 1.0; b->plays = b->stops = b->reissues = 0;
        b->wr_bytes = b->wr_calls = b->wr_nonzero = 0;
        g_pcm_mirror_bytes += bytes;
    }
    pthread_mutex_unlock(&g_slock);

    if (!id){
        free(pcm);
        audio_log_register_failure_once(bytes);
        return 0;
    }
    fprintf(stderr, "  [ses] tampon #%d: %u Hz, %u kanal, %u bit, %u bayt (%.2f sn)\n",
            id, rate, chans, bits, bytes,
            (double)bytes / (double)(rate * chans * (bits/8u)));
    return id;
}

void audio_play(int id, int loop){
    if (id <= 0 || id >= MAXSND) return;
    pthread_mutex_lock(&g_slock);
    if (g_snd[id].used){
        /* DirectSound Play baslangic imlecinden devam eder. Stop sonrasinda
         * imlec korunur; dogal one-shot sonu zaten mix_into tarafindan 0'a
         * alindigi icin tamamlanmis bir ses sonraki Play'de bastan baslar. */
        if (!g_snd[id].playing) audio_count(&g_replay);
        else { audio_count(&g_reissue); audio_count(&g_snd[id].reissues); }
        g_snd[id].playing = 1;
        g_snd[id].loop = loop;
        audio_count(&g_snd[id].plays);
    }
    pthread_mutex_unlock(&g_slock);
}

void audio_stop(int id){
    if (id <= 0 || id >= MAXSND) return;
    pthread_mutex_lock(&g_slock);
    if (g_snd[id].used){
        g_snd[id].playing = 0;
        g_snd[id].stream_writes = 0;
        audio_count(&g_snd[id].stops);
    }
    pthread_mutex_unlock(&g_slock);
}

int audio_set_position(int id, uint32_t byte_pos){
    if (id <= 0 || id >= MAXSND) return 0;
    int ok = 0;
    pthread_mutex_lock(&g_slock);
    SndBuf *b = &g_snd[id];
    uint32_t bpf = b->chans * (b->bits / 8u);
    if (b->used && b->bytes && bpf && byte_pos < b->bytes){
        b->cursor = (double)byte_pos / (double)bpf;
        b->stream_writes = 0;
        ok = 1;
    }
    pthread_mutex_unlock(&g_slock);
    return ok;
}

uint32_t audio_status(int id){
    if (id <= 0 || id >= MAXSND) return 0;
    uint32_t status = 0;
    pthread_mutex_lock(&g_slock);
    const SndBuf *b = &g_snd[id];
    if (b->used && b->playing){
        status = AUDIO_STATUS_PLAYING;
        if (b->loop) status |= AUDIO_STATUS_LOOPING;
    }
    pthread_mutex_unlock(&g_slock);
    return status;
}

/* One short try-lock keeps diagnostics from waiting behind the real-time
 * mixer. Return zero when a coherent buffer snapshot is not immediately
 * available; callers can simply skip that sample. */
int audio_snapshot(AudioSnapshot *out){
    if (!out) return 0;
    uint64_t now = audio_monotonic_us();
    if (pthread_mutex_trylock(&g_slock) != 0) return 0;
    AudioSnapshot snap = {0};
    for (int i = 1; i < g_sndn; i++){
        const SndBuf *b = &g_snd[i];
        if (!b->used) continue;
        snap.buffer_count++;
        if (b->playing) audio_add(&snap.active_buffers, 1);
        audio_add(&snap.plays, b->plays);
        audio_add(&snap.reissues, b->reissues);
        audio_add(&snap.stops, b->stops);
        audio_add(&snap.write_calls, b->wr_calls);
    }

    /* Reserve the bounded rows for currently active buffers first. */
    for (unsigned pass = 0; pass < 2 &&
         snap.buffers_captured < AUDIO_SNAPSHOT_MAX_BUFFERS; pass++){
        for (int i = 1; i < g_sndn &&
             snap.buffers_captured < AUDIO_SNAPSHOT_MAX_BUFFERS; i++){
            const SndBuf *b = &g_snd[i];
            if (!b->used || (pass == 0) != (b->playing != 0)) continue;
            AudioBufferSnapshot *row = &snap.buffers[snap.buffers_captured++];
            row->id = (uint32_t)i;
            row->storage_id = b->storage_id;
            row->size_bytes = b->bytes;
            row->rate = b->rate;
            row->channels = b->chans;
            row->bits = b->bits;
            row->active = b->playing ? 1u : 0u;
            row->looping = b->loop ? 1u : 0u;
            row->write_seen = b->diag_write_seen ? 1u : 0u;
            audio_cursors_locked(b, &row->play_cursor_bytes, &row->write_cursor_bytes);
            row->write_end_offset = b->diag_write_end_offset;
            row->last_write_bytes_reported = b->diag_last_write_bytes;
            row->write_calls = b->diag_write_calls;
            row->write_bytes_reported = b->diag_write_bytes;
            row->last_write_age_us = (b->diag_write_seen && b->diag_last_write_us &&
                                      now >= b->diag_last_write_us)
                                   ? now - b->diag_last_write_us : 0;
            row->max_write_gap_us = b->diag_max_write_gap_us;
        }
    }
    snap.buffers_omitted = snap.buffer_count - snap.buffers_captured;
    snap.hardware_rate = audio_get_hardware_rate();
    snap.callback_count = atomic_load_explicit(&g_cb_count, memory_order_relaxed);
    snap.callback_gap_max_us = atomic_load_explicit(&g_cb_gap_max_us, memory_order_relaxed);
    snap.callback_duration_max_us = atomic_load_explicit(&g_cb_duration_max_us, memory_order_relaxed);
    snap.callback_late_count = atomic_load_explicit(&g_cb_late_count, memory_order_relaxed);
    snap.callback_over_budget_count = atomic_load_explicit(&g_cb_over_budget_count, memory_order_relaxed);
    snap.callback_max_frames = (uint32_t)atomic_load_explicit(&g_cb_max_frames, memory_order_relaxed);
    snap.mirror_bytes = g_pcm_mirror_bytes;
    snap.publish_calls = g_pcm_publish_calls;
    snap.publish_bytes = g_pcm_publish_bytes;
    snap.publish_max_ns = g_pcm_publish_max_ns;
    snap.rejected_writes = g_pcm_rejected_writes;
    *out = snap;
    pthread_mutex_unlock(&g_slock);
    return 1;
}

void audio_note_write(int id, uint32_t end_off){
    if (id <= 0 || id >= MAXSND) return;
    pthread_mutex_lock(&g_slock);
    if (g_snd[id].used && g_snd[id].bytes){
        g_snd[id].wpos = end_off % g_snd[id].bytes;
        g_snd[id].wseen = 1;
        uint32_t storage_id = g_snd[id].storage_id;
        for (int i = 1; i < g_sndn; i++){
            SndBuf *b = &g_snd[i];
            if (b->used && b->bytes && b->storage_id == storage_id){
                b->diag_write_end_offset = end_off % b->bytes;
            }
        }
    }
    pthread_mutex_unlock(&g_slock);
}

void audio_note_write_stat(int id, uint32_t n, int nonzero){
    if (id <= 0 || id >= MAXSND) return;
    uint64_t now = audio_monotonic_us();
    pthread_mutex_lock(&g_slock);
    if (g_snd[id].used){
        SndBuf *target = &g_snd[id];
        audio_count(&target->wr_calls);
        audio_add(&target->wr_bytes, n);
        if (nonzero) audio_count(&target->wr_nonzero);
        uint32_t storage_id = target->storage_id;
        for (int i = 1; i < g_sndn; i++){
            SndBuf *b = &g_snd[i];
            if (!b->used || b->storage_id != storage_id) continue;
            if (b->playing && n > 0 && n < b->bytes){
                if (b->stream_writes != UINT32_MAX) b->stream_writes++;
            }
            if (now && b->diag_last_write_us && now >= b->diag_last_write_us){
                uint64_t gap = now - b->diag_last_write_us;
                if (gap > b->diag_max_write_gap_us) b->diag_max_write_gap_us = gap;
            }
            b->diag_write_seen = 1;
            b->diag_last_write_us = now;
            b->diag_last_write_bytes = n;
            audio_count(&b->diag_write_calls);
            audio_add(&b->diag_write_bytes, n);
        }
    }
    pthread_mutex_unlock(&g_slock);
}

static int audio_pcm_region_offset(const SndBuf *b, uint32_t guest_addr,
                                   uint32_t bytes, uint32_t *offset){
    if (!bytes){
        *offset = 0;
        return 1;
    }
    if (guest_addr < b->data) return 0;
    uint32_t delta = guest_addr - b->data;
    if (delta > b->bytes || bytes > b->bytes - delta) return 0;
    *offset = delta;
    return 1;
}

int audio_publish_write(int id, uint32_t p1, uint32_t n1,
                        uint32_t p2, uint32_t n2, int nonzero_sample){
    uint64_t started_ns = audio_monotonic_ns();
    int ok = 0;
    pthread_mutex_lock(&g_slock);
    if (id > 0 && id < g_sndn){
        SndBuf *target = &g_snd[id];
        uint32_t off1 = 0, off2 = 0;
        int valid1 = target->used && target->pcm && g_mem &&
                     audio_pcm_region_offset(target, p1, n1, &off1);
        int valid2 = target->used && target->pcm && g_mem &&
                     audio_pcm_region_offset(target, p2, n2, &off2);
        /* Validate both guest spans completely before publishing either one. */
        if (valid1 && valid2){
            if (n1) memcpy(target->pcm + off1, g_mem + p1, n1);
            if (n2) memcpy(target->pcm + off2, g_mem + p2, n2);

            uint64_t total_bytes = (uint64_t)n1 + (uint64_t)n2;
            uint32_t end_offset = target->diag_write_end_offset;
            if (total_bytes){
                uint64_t now_us;
                uint64_t raw_end = n2 ? (uint64_t)off2 + n2 : (uint64_t)off1 + n1;
                end_offset = (uint32_t)(raw_end % target->bytes);
                target->wpos = end_offset;
                target->wseen = 1;
                audio_count(&target->wr_calls);
                audio_add(&target->wr_bytes, total_bytes);
                if (nonzero_sample > 0) audio_count(&target->wr_nonzero);
                /* Age/gap timestamps refer to the completed mirror copy. */
                now_us = audio_monotonic_us();

                uint32_t reported_bytes = total_bytes > UINT32_MAX
                                        ? UINT32_MAX : (uint32_t)total_bytes;
                for (int i = 1; i < g_sndn; i++){
                    SndBuf *b = &g_snd[i];
                    if (!b->used || b->storage_id != target->storage_id) continue;
                    if (b->playing && reported_bytes > 0 && reported_bytes < b->bytes){
                        if (b->stream_writes != UINT32_MAX) b->stream_writes++;
                    }
                    b->wpos = end_offset;
                    b->wseen = 1;
                    b->diag_write_end_offset = end_offset;
                    if (now_us && b->diag_last_write_us &&
                        now_us >= b->diag_last_write_us){
                        uint64_t gap = now_us - b->diag_last_write_us;
                        if (gap > b->diag_max_write_gap_us) b->diag_max_write_gap_us = gap;
                    }
                    b->diag_write_seen = 1;
                    b->diag_last_write_us = now_us;
                    b->diag_last_write_bytes = reported_bytes;
                    audio_count(&b->diag_write_calls);
                    audio_add(&b->diag_write_bytes, total_bytes);
                }
                audio_count(&g_pcm_publish_calls);
                audio_add(&g_pcm_publish_bytes, total_bytes);
                uint64_t finished_ns = audio_monotonic_ns();
                uint64_t elapsed_ns = started_ns && finished_ns >= started_ns
                                    ? finished_ns - started_ns : 0;
                if (elapsed_ns > g_pcm_publish_max_ns) g_pcm_publish_max_ns = elapsed_ns;
            }
            ok = 1;
        }
    }
    if (!ok) audio_count(&g_pcm_rejected_writes);
    pthread_mutex_unlock(&g_slock);
    return ok;
}

void audio_set_freq(int id, uint32_t hz){
    if (id <= 0 || id >= MAXSND) return;
    pthread_mutex_lock(&g_slock);
    if (g_snd[id].used && g_snd[id].rate){
        if (!hz) g_snd[id].freq = 1.0;
        else {
            double f = (double)hz / (double)g_snd[id].rate;
            if (f < 0.05) f = 0.05;
            if (f > 8.00) f = 8.00;
            g_snd[id].freq = f;
        }
    }
    pthread_mutex_unlock(&g_slock);
}

uint32_t audio_get_freq(int id){
    if (id <= 0 || id >= MAXSND) return 0;
    uint32_t hz = 0;
    pthread_mutex_lock(&g_slock);
    const SndBuf *b = &g_snd[id];
    if (b->used && b->rate){
        double actual = (double)b->rate * b->freq;
        if (actual > 0.0 && actual < 4294967295.0) hz = (uint32_t)llround(actual);
    }
    pthread_mutex_unlock(&g_slock);
    return hz;
}

int audio_duplicate(int src){
    if (src <= 0 || src >= MAXSND) return 0;
    int id = 0;
    pthread_mutex_lock(&g_slock);
    if (g_snd[src].used && g_sndn < MAXSND){
        id = g_sndn++;
        g_snd[id] = g_snd[src];
        g_snd[id].playing = 0; g_snd[id].cursor = 0.0; g_snd[id].trim = 1.0;
        g_snd[id].target = 0.0; g_snd[id].over = 0;
        g_snd[id].stream_writes = 0;
        g_snd[id].plays = g_snd[id].stops = g_snd[id].reissues = 0;
        g_snd[id].wr_bytes = g_snd[id].wr_calls = g_snd[id].wr_nonzero = 0;
    }
    pthread_mutex_unlock(&g_slock);
    return id;
}

static void audio_cursors_locked(const SndBuf *b, uint32_t *play, uint32_t *write){
    uint32_t p = 0, w = 0;
    if (b && b->used && b->bytes){
        uint32_t bpf = b->chans * (b->bits / 8u);
        uint32_t sz = b->bytes;
        if (bpf){
            uint32_t raw_p = (uint32_t)(b->cursor * (double)bpf + 1e-6);
            p = ((raw_p / bpf) * bpf) % sz;
            uint32_t lead = (b->rate * WRITE_LEAD_MS / 1000u) * bpf;
            if (lead >= sz) lead = ((sz / 4u) / bpf) * bpf;
            if (!lead && sz >= bpf) lead = bpf;
            w = (((p + lead) / bpf) * bpf) % sz;
        }
    }
    if (play) *play = p;
    if (write) *write = w;
}

void audio_cursors(int id, uint32_t *play, uint32_t *write){
    uint32_t p = 0, w = 0;
    if (id > 0 && id < MAXSND){
        pthread_mutex_lock(&g_slock);
        audio_cursors_locked(&g_snd[id], &p, &w);
        pthread_mutex_unlock(&g_slock);
    }
    if (play)  *play  = p;
    if (write) *write = w;
}

uint32_t audio_cursor(int id){
    uint32_t p = 0;
    audio_cursors(id, &p, NULL);
    return p;
}

void audio_set_volume(int id, long millibel){
    if (id <= 0 || id >= MAXSND) return;
    float v = (millibel <= -10000L) ? 0.f : powf(10.f, (float)millibel / 2000.f);
    if (v > 1.f) v = 1.f;
    pthread_mutex_lock(&g_slock);
    if (g_snd[id].used) g_snd[id].vol = v;
    pthread_mutex_unlock(&g_slock);
}

double audio_time(void){
    uint32_t hw_rate = (uint32_t)atomic_load_explicit(&g_hw_rate, memory_order_relaxed);
    if (!hw_rate) hw_rate = AUDIO_DEFAULT_RATE;
    return (double)g_mixed_frames / (double)hw_rate;
}

uint32_t audio_get_hardware_rate(void){
    return (uint32_t)atomic_load_explicit(&g_hw_rate, memory_order_relaxed);
}

int audio_is_active(void){
    return atomic_load_explicit(&g_audio_on, memory_order_relaxed) &&
           !atomic_load_explicit(&g_audio_suspended, memory_order_relaxed);
}

int audio_is_suspended(void){
    return atomic_load_explicit(&g_audio_suspended, memory_order_relaxed);
}

/* --- Karistirici (Mixer) Cekirdegi --- */
static void mix_chunk(int16_t *out, unsigned frames, uint32_t hw_rate){
    static float acc[FRAMES * OUT_CH];
    memset(acc, 0, sizeof(float) * frames * OUT_CH);
    uint64_t active = 0;

    pthread_mutex_lock(&g_slock);
    uint64_t now_us = audio_monotonic_us();
    for (int i = 1; i < g_sndn; i++){
        SndBuf *b = &g_snd[i];
        if (!b->used || !b->playing || !b->bytes || !b->pcm) continue;
        uint32_t bps  = b->bits / 8u;
        uint32_t bpf  = b->chans * bps;
        if (!bpf) continue;
        uint32_t nsrc = b->bytes / bpf;
        if (!nsrc) continue;

        /* Eski masaustu backend'i icin yazma imleci servosu. iOS'ta guest
         * saati/sahibi belirsiz yazma konumu pitch'i degistirmemeli. */
#if !TARGET_OS_IPHONE && !defined(NFSU2_AUDIO_CORE_TEST)
        if (b->wseen){
            uint32_t bpf2 = b->chans * (b->bits/8u);
            uint32_t pbyte = ((uint32_t)(b->cursor * (double)bpf2)) % b->bytes;
            uint32_t avail = (b->wpos + b->bytes - pbyte) % b->bytes;
            double frac = (double)avail / (double)b->bytes;
            double want;
            if (frac > 0.85){
                audio_count(&b->over);
                want = 0.94;
            } else {
                if (b->target <= 0.0) b->target = frac;
                else b->target += (frac - b->target) * 0.0008;
                if (b->target < 0.12) b->target = 0.12;
                if (b->target > 0.60) b->target = 0.60;
                want = 1.0 + (frac - b->target) * 1.5;
                if (want < 0.97) want = 0.97;
                if (want > 1.03) want = 1.03;
            }
            b->trim += (want - b->trim) * 0.05;
        } else {
            b->trim += (1.0 - b->trim) * 0.08;
        }
        if (b->trim < 0.5 || b->trim > 2.0) b->trim = 1.0;
#else
        b->trim = 1.0;
#endif

        /* Adim: kaynak/donanim orani * saat orani * servo * SetFrequency. */
        double step = ((double)b->rate / (double)hw_rate) * g_rate_ratio * b->trim * b->freq;
        active++;

        int stalled_stream = 0;
        if (b->loop && b->stream_writes >= 2u && b->diag_last_write_us > 0 &&
            now_us > b->diag_last_write_us){
            uint64_t buf_us = (uint64_t)nsrc * 1000000ull / (b->rate ? b->rate : AUDIO_DEFAULT_RATE);
            if (buf_us < 250000ull) buf_us = 250000ull;
            if (now_us - b->diag_last_write_us > buf_us){
                stalled_stream = 1;
                audio_count(&b->over);
            }
        }

        if (!stalled_stream){
            if (b->bits == 16u && b->chans == 2u){
                const int16_t *pcm16 = (const int16_t *)b->pcm;
                const float vol_scale = b->vol * (1.0f / 32768.0f);
                for (unsigned f = 0; f < frames; f++){
                    double sp = b->cursor + step * (double)f;
                    if (sp >= (double)nsrc){
                        if (b->loop){
                            double source_len = (double)nsrc;
                            if (sp < source_len * 2.0) sp -= source_len;
                            else sp = fmod(sp, source_len);
                        }
                        else { b->playing = 0; break; }
                    }
                    uint32_t si0 = (uint32_t)sp;
                    uint32_t si1 = (si0 + 1 < nsrc) ? (si0 + 1) : (b->loop ? 0 : si0);
                    float alpha = (float)(sp - (double)si0);

                    size_t idx0 = (size_t)si0 * 2u;
                    size_t idx1 = (size_t)si1 * 2u;

                    float s0_l = (float)pcm16[idx0];
                    float s1_l = (float)pcm16[idx1];
                    acc[f * OUT_CH + 0] += (s0_l + alpha * (s1_l - s0_l)) * vol_scale;

                    float s0_r = (float)pcm16[idx0 + 1u];
                    float s1_r = (float)pcm16[idx1 + 1u];
                    acc[f * OUT_CH + 1] += (s0_r + alpha * (s1_r - s0_r)) * vol_scale;
                }
            } else if (b->bits == 16u && b->chans == 1u){
                const int16_t *pcm16 = (const int16_t *)b->pcm;
                const float vol_scale = b->vol * (1.0f / 32768.0f);
                for (unsigned f = 0; f < frames; f++){
                    double sp = b->cursor + step * (double)f;
                    if (sp >= (double)nsrc){
                        if (b->loop){
                            double source_len = (double)nsrc;
                            if (sp < source_len * 2.0) sp -= source_len;
                            else sp = fmod(sp, source_len);
                        }
                        else { b->playing = 0; break; }
                    }
                    uint32_t si0 = (uint32_t)sp;
                    uint32_t si1 = (si0 + 1 < nsrc) ? (si0 + 1) : (b->loop ? 0 : si0);
                    float alpha = (float)(sp - (double)si0);

                    float s0 = (float)pcm16[si0];
                    float s1 = (float)pcm16[si1];
                    float sample = (s0 + alpha * (s1 - s0)) * vol_scale;
                    acc[f * OUT_CH + 0] += sample;
                    acc[f * OUT_CH + 1] += sample;
                }
            } else {
                for (unsigned f = 0; f < frames; f++){
                    double sp = b->cursor + step * (double)f;
                    if (sp >= (double)nsrc){
                        if (b->loop){
                            double source_len = (double)nsrc;
                            if (sp < source_len * 2.0) sp -= source_len;
                            else sp = fmod(sp, source_len);
                        }
                        else { b->playing = 0; break; }
                    }
                    uint32_t si0 = (uint32_t)sp;
                    uint32_t si1 = (si0 + 1 < nsrc) ? (si0 + 1) : (b->loop ? 0 : si0);
                    float alpha = (float)(sp - (double)si0);

                    for (unsigned c = 0; c < OUT_CH; c++){
                        uint32_t sc = (b->chans == 2u) ? (uint32_t)c : 0u;
                        size_t off0 = (size_t)si0 * bpf + sc * bps;
                        size_t off1 = (size_t)si1 * bpf + sc * bps;
                        float s0, s1;
                        if (b->bits == 16u){
                            int16_t v0, v1;
                            memcpy(&v0, b->pcm + off0, 2);
                            memcpy(&v1, b->pcm + off1, 2);
                            s0 = (float)v0 / 32768.f;
                            s1 = (float)v1 / 32768.f;
                        } else {
                            s0 = ((float)b->pcm[off0] - 128.f) / 128.f;
                            s1 = ((float)b->pcm[off1] - 128.f) / 128.f;
                        }
                        float sample = s0 + alpha * (s1 - s0);
                        acc[f * OUT_CH + c] += sample * b->vol;
                    }
                }
            }
        }

        b->cursor += step * (double)frames;
        if (nsrc){
            if (b->cursor >= (double)nsrc){
                if (b->loop){
                    double source_len = (double)nsrc;
                    if (b->cursor < source_len * 2.0) b->cursor -= source_len;
                    else b->cursor = fmod(b->cursor, source_len);
                }
                else { b->cursor = 0.0; b->playing = 0; }
            }
        }
    }
    pthread_mutex_unlock(&g_slock);

    if (active > g_active_peak) g_active_peak = active;
    g_mixed_frames += frames;

    /* Doygunluk, exact-zero diagnostics, and int16 packing share one pass. */
    unsigned zero = 0;
    for (unsigned k = 0; k < frames * OUT_CH; k++){
        float v = acc[k];
        if (active && v == 0.f) zero++;
        if (v >  1.f) v =  1.f;
        if (v < -1.f) v = -1.f;
        out[k] = (int16_t)(v * 32767.f);
    }
    /* Keep the original active-buffer guard and pre-clamp exact-zero test. */
    if (active){
        g_out_frames += frames * OUT_CH;
        g_out_zero   += zero;
        if (zero > (frames * OUT_CH) * 3u / 4u) g_gap_cb++;
    }
    g_cb_no++;
    if (g_dump) fwrite(out, sizeof(int16_t), (size_t)frames * OUT_CH, g_dump);
}

/* AudioUnit may request more than FRAMES despite MaximumFramesPerSlice. Mix
 * the complete request in bounded chunks instead of leaving the tail stale. */
static void mix_into(int16_t *out, unsigned frames){
    if (!out || !frames) return;
    update_rate_ratio();
    uint32_t hw_rate = (uint32_t)atomic_load_explicit(&g_hw_rate, memory_order_relaxed);
    if (!hw_rate) hw_rate = AUDIO_DEFAULT_RATE;
    while (frames){
        unsigned chunk = frames > FRAMES ? FRAMES : frames;
        mix_chunk(out, chunk, hw_rate);
        out += (size_t)chunk * OUT_CH;
        frames -= chunk;
    }
}

#ifndef NFSU2_AUDIO_CORE_TEST
static uint64_t now_us_a(void){
    return audio_monotonic_us();
}

/* One clock sample at callback entry and one at exit; never called per sample. */
static uint64_t audio_callback_begin(uint64_t *last, uint64_t *last_budget,
                                    uint32_t frames, uint32_t hw_rate,
                                    uint64_t *budget_out){
    if (atomic_exchange_explicit(&g_cb_clock_reset, false, memory_order_relaxed)){
        *last = 0;
        *last_budget = 0;
    }
    uint64_t now = now_us_a();
    uint64_t budget = hw_rate ? (uint64_t)frames * 1000000ull / hw_rate : 0;
    uint64_t gap = 0;
    if (*last && now >= *last){
        gap = now - *last;
        if (gap > g_worst_gap_us) g_worst_gap_us = gap;
        audio_atomic_max_u64(&g_cb_gap_max_us, gap);
        /* A callback interval follows the prior callback's frame count. */
        if (*last_budget && gap > *last_budget * 2u){
            audio_count(&g_late_callbacks);
            atomic_fetch_add_explicit(&g_cb_late_count, 1, memory_order_relaxed);
        }
    }
    *last = now;
    *last_budget = budget;
    atomic_fetch_add_explicit(&g_cb_count, 1, memory_order_relaxed);
    audio_atomic_max_u32(&g_cb_max_frames, frames);
    if (budget_out) *budget_out = budget;
    return gap;
}

static void audio_callback_end(uint64_t started, uint64_t budget){
    uint64_t ended = now_us_a();
    if (!started || ended < started) return;
    uint64_t duration = ended - started;
    audio_atomic_max_u64(&g_cb_duration_max_us, duration);
    if (budget && duration > budget)
        atomic_fetch_add_explicit(&g_cb_over_budget_count, 1, memory_order_relaxed);
}

/* AudioQueue Geri Cagirmasi (Yedek Yol) */
static void cb(void *ud, AudioQueueRef q, AudioQueueBufferRef buf){
    (void)ud;
    if (atomic_load_explicit(&g_audio_suspended, memory_order_relaxed)){
        atomic_store_explicit(&g_cb_clock_reset, true, memory_order_relaxed);
        memset(buf->mAudioData, 0, FRAMES * OUT_CH * sizeof(int16_t));
        buf->mAudioDataByteSize = FRAMES * OUT_CH * sizeof(int16_t);
        AudioQueueEnqueueBuffer(q, buf, 0, NULL);
        return;
    }
    uint32_t hw_rate = (uint32_t)atomic_load_explicit(&g_hw_rate, memory_order_relaxed);
    if (!hw_rate) hw_rate = AUDIO_DEFAULT_RATE;

    static uint64_t last = 0;
    static uint64_t last_budget = 0;
    uint64_t budget = 0;
    uint64_t gap = audio_callback_begin(&last, &last_budget, FRAMES, hw_rate, &budget);
    if (gap && budget && gap > budget * (uint64_t)NBUF) audio_count(&g_starved);

    mix_into((int16_t *)buf->mAudioData, FRAMES);
    buf->mAudioDataByteSize = FRAMES * OUT_CH * sizeof(int16_t);
    AudioQueueEnqueueBuffer(q, buf, 0, NULL);
    audio_callback_end(last, budget);
}

/* RemoteIO AudioUnit render callback. mix_into/mix_chunk reads host PCM mirrors and
 * takes g_slock, so this is not a lock-free callback path. */
static OSStatus au_cb(void *ud, AudioUnitRenderActionFlags *flags,
                      const AudioTimeStamp *ts, UInt32 bus, UInt32 nframes,
                      AudioBufferList *abl){
    (void)ud; (void)flags; (void)ts; (void)bus;
    if (!abl || abl->mNumberBuffers < 1 || !abl->mBuffers[0].mData) return noErr;

    /* Askiya alma veya rota rekonfigrasyonu sirasinda koruma: sessizlik ver ve don */
    if (atomic_load_explicit(&g_audio_suspended, memory_order_relaxed) ||
        atomic_load_explicit(&g_audio_reconfiguring, memory_order_relaxed) ||
        !atomic_load_explicit(&g_audio_on, memory_order_relaxed)){
        atomic_store_explicit(&g_cb_clock_reset, true, memory_order_relaxed);
        memset(abl->mBuffers[0].mData, 0, nframes * OUT_CH * sizeof(int16_t));
        abl->mBuffers[0].mDataByteSize = nframes * OUT_CH * sizeof(int16_t);
        return noErr;
    }

    static uint64_t last = 0;
    static uint64_t last_budget = 0;
    uint32_t hw_rate = (uint32_t)atomic_load_explicit(&g_hw_rate, memory_order_relaxed);
    if (!hw_rate) hw_rate = AUDIO_DEFAULT_RATE;
    uint64_t budget = 0;
    (void)audio_callback_begin(&last, &last_budget,
                               (uint32_t)nframes, hw_rate, &budget);

    int16_t *out = (int16_t *)abl->mBuffers[0].mData;
    if (nframes > g_au_slice) g_au_slice = nframes;

    mix_into(out, (unsigned)nframes);
    abl->mBuffers[0].mDataByteSize = nframes * OUT_CH * sizeof(int16_t);
    audio_callback_end(last, budget);
    return noErr;
}

#if TARGET_OS_IPHONE

/* --- iOS AVAudioSession ve Yasam Dongusu Gozlemcisi --- */
@interface NFSU2AudioLifecycleObserver : NSObject
@end

@implementation NFSU2AudioLifecycleObserver

- (instancetype)init {
    self = [super init];
    if (self) {
        NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
        AVAudioSession *session = [AVAudioSession sharedInstance];

        [nc addObserver:self
               selector:@selector(handleInterruption:)
                   name:AVAudioSessionInterruptionNotification
                 object:session];

        [nc addObserver:self
               selector:@selector(handleRouteChange:)
                   name:AVAudioSessionRouteChangeNotification
                 object:session];

        [nc addObserver:self
               selector:@selector(handleMediaReset:)
                   name:AVAudioSessionMediaServicesWereResetNotification
                 object:session];

#if !TARGET_OS_SIMULATOR && !TARGET_OS_MACCATALYST
        [nc addObserver:self
               selector:@selector(handleAppDidEnterBackground:)
                   name:UIApplicationDidEnterBackgroundNotification
                 object:nil];

        [nc addObserver:self
               selector:@selector(handleAppWillEnterForeground:)
                   name:UIApplicationWillEnterForegroundNotification
                 object:nil];
#endif
    }
    return self;
}

- (void)handleInterruption:(NSNotification *)note {
    NSDictionary *info = note.userInfo;
    if (!info) return;
    AVAudioSessionInterruptionType type = (AVAudioSessionInterruptionType)[[info objectForKey:AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];

    if (type == AVAudioSessionInterruptionTypeBegan) {
        fprintf(stderr, "  [ses-ios] Kesinti basladi (Telefon/Alarm/Siri), ses guvenle durduruluyor\n");
        audio_ios_pause();
    } else if (type == AVAudioSessionInterruptionTypeEnded) {
        NSUInteger opts = [[info objectForKey:AVAudioSessionInterruptionOptionKey] unsignedIntegerValue];
        fprintf(stderr, "  [ses-ios] Kesinti bitti, surdurulme bayragi=%d\n",
                (opts & AVAudioSessionInterruptionOptionShouldResume) ? 1 : 0);
        if (opts & AVAudioSessionInterruptionOptionShouldResume) {
            audio_ios_resume();
        }
    }
}

- (void)handleRouteChange:(NSNotification *)note {
    NSDictionary *info = note.userInfo;
    if (!info) return;
    AVAudioSessionRouteChangeReason reason = (AVAudioSessionRouteChangeReason)[[info objectForKey:AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue];
    fprintf(stderr, "  [ses-ios] Rota degisikligi algilandi (sebep: %u)\n", (unsigned)reason);

    AVAudioSession *session = [AVAudioSession sharedInstance];
    double currentHW = session.sampleRate;
    if (currentHW > 4000.0 && currentHW < 192000.0) {
        uint32_t target_rate = (uint32_t)round(currentHW);
        uint32_t current_rate = (uint32_t)atomic_load_explicit(&g_hw_rate, memory_order_relaxed);
        if (target_rate != current_rate) {
            fprintf(stderr, "  [ses-ios] Donanim ornekleme hizi degisimi: %u Hz -> %u Hz\n",
                    current_rate, target_rate);
            audio_ios_reconfigure_sample_rate(target_rate);
        }
    }
}

- (void)handleMediaReset:(NSNotification *)note {
    (void)note;
    fprintf(stderr, "  [ses-ios] KRITIK: audiod MediaServicesWereReset! Ses hatti bastan kuruluyor.\n");
    audio_ios_restart_subsystem();
}

#if !TARGET_OS_SIMULATOR && !TARGET_OS_MACCATALYST
- (void)handleAppDidEnterBackground:(NSNotification *)note {
    (void)note;
    fprintf(stderr, "  [ses-ios] Uygulama arka plana gecti, RemoteIO durduruluyor\n");
    audio_ios_pause();
}

- (void)handleAppWillEnterForeground:(NSNotification *)note {
    (void)note;
    fprintf(stderr, "  [ses-ios] Uygulama on plana dondu, RemoteIO canlandiriliyor\n");
    audio_ios_resume();
}
#endif

@end

static NFSU2AudioLifecycleObserver *g_lifecycle_observer = nil;

static int audio_setup_ios_session(void){
    @autoreleasepool {
        AVAudioSession *session = [AVAudioSession sharedInstance];
        NSError *err = nil;

        BOOL ok = [session setCategory:AVAudioSessionCategoryPlayback
                           withOptions:AVAudioSessionCategoryOptionMixWithOthers
                                 error:&err];
        if (!ok || err){
            fprintf(stderr, "  [ses-ios] AVAudioSession setCategory hatasi: %s\n",
                    err ? [[err localizedDescription] UTF8String] : "bilinmiyor");
            return 0;
        }

        /* 5-10 ms ultra dusuk gecikmeli donanim tamponu */
        [session setPreferredIOBufferDuration:0.008 error:&err];
        if (err) err = nil;

        /* Donanim ornekleme hizi tercihi: 44.1 kHz */
        [session setPreferredSampleRate:(double)AUDIO_DEFAULT_RATE error:&err];
        if (err) err = nil;

        ok = [session setActive:YES error:&err];
        if (!ok || err){
            fprintf(stderr, "  [ses-ios] AVAudioSession setActive hatasi: %s\n",
                    err ? [[err localizedDescription] UTF8String] : "bilinmiyor");
            return 0;
        }

        /* Gercek donanim ornekleme hizini sorgula (AirPods 48kHz, Dahili 44.1kHz/48kHz) */
        double actRate = session.sampleRate;
        if (actRate >= 4000.0 && actRate <= 192000.0){
            atomic_store_explicit(&g_hw_rate, (uint32_t)round(actRate), memory_order_relaxed);
        } else {
            atomic_store_explicit(&g_hw_rate, AUDIO_DEFAULT_RATE, memory_order_relaxed);
        }

        fprintf(stderr, "  [ses-ios] AVAudioSession baslatildi (Gercek Donanim: %u Hz, IO: %.1f ms)\n",
                (uint32_t)atomic_load_explicit(&g_hw_rate, memory_order_relaxed),
                session.IOBufferDuration * 1000.0);

        if (!g_lifecycle_observer){
            g_lifecycle_observer = [[NFSU2AudioLifecycleObserver alloc] init];
        }
        return 1;
    }
}
#endif

/* RemoteIO AudioUnit Baslatma */
static int au_start(uint32_t rate){
    AudioComponentDescription d;
    memset(&d, 0, sizeof d);
    d.componentType         = kAudioUnitType_Output;
#if TARGET_OS_IPHONE
    d.componentSubType      = kAudioUnitSubType_RemoteIO;
#else
    d.componentSubType      = kAudioUnitSubType_DefaultOutput;
#endif
    d.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent comp = AudioComponentFindNext(NULL, &d);
    if (!comp) return 0;
    if (AudioComponentInstanceNew(comp, &g_au) != noErr) return 0;

#if TARGET_OS_IPHONE
    {
        UInt32 enableOutput = 1;
        AudioUnitSetProperty(g_au, kAudioOutputUnitProperty_EnableIO,
                             kAudioUnitScope_Output, 0, &enableOutput, sizeof enableOutput);
    }
#endif

    AudioStreamBasicDescription f;
    memset(&f, 0, sizeof f);
    f.mSampleRate       = (double)rate;
    f.mFormatID         = kAudioFormatLinearPCM;
    f.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = OUT_CH;
    f.mBitsPerChannel   = 16;
    f.mFramesPerPacket  = 1;
    f.mBytesPerFrame    = OUT_CH * sizeof(int16_t);
    f.mBytesPerPacket   = f.mBytesPerFrame;

    if (AudioUnitSetProperty(g_au, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input, 0, &f, sizeof f) != noErr) goto fail;

    {
        UInt32 mx = FRAMES;
        AudioUnitSetProperty(g_au, kAudioUnitProperty_MaximumFramesPerSlice,
                             kAudioUnitScope_Global, 0, &mx, sizeof mx);
    }

    {
        AURenderCallbackStruct rc;
        rc.inputProc = au_cb;
        rc.inputProcRefCon = NULL;
        if (AudioUnitSetProperty(g_au, kAudioUnitProperty_SetRenderCallback,
                                 kAudioUnitScope_Input, 0, &rc, sizeof rc) != noErr) goto fail;
    }

    if (AudioUnitInitialize(g_au) != noErr) goto fail;
    if (AudioOutputUnitStart(g_au) != noErr){
        AudioUnitUninitialize(g_au);
        goto fail;
    }
    return 1;

fail:
    if (g_au){
        AudioComponentInstanceDispose(g_au);
        g_au = NULL;
    }
    return 0;
}

/* --- Yasam Dongusu API Fonksiyonlari --- */

void audio_ios_pause(void){
    atomic_store_explicit(&g_cb_clock_reset, true, memory_order_relaxed);
    atomic_store_explicit(&g_audio_suspended, true, memory_order_release);
    if (g_backend == 1 && g_au){
        AudioOutputUnitStop(g_au);
    } else if (g_backend == 2 && g_q){
        AudioQueuePause(g_q);
    }
}

void audio_ios_resume(void){
    atomic_store_explicit(&g_cb_clock_reset, true, memory_order_relaxed);
#if TARGET_OS_IPHONE
    @autoreleasepool {
        NSError *err = nil;
        [[AVAudioSession sharedInstance] setActive:YES error:&err];
    }
#endif
    if (g_backend == 1 && g_au){
        AudioOutputUnitStart(g_au);
    } else if (g_backend == 2 && g_q){
        AudioQueueStart(g_q, NULL);
    }
    atomic_store_explicit(&g_audio_suspended, false, memory_order_release);
}

void audio_ios_reconfigure_sample_rate(uint32_t new_rate){
    if (new_rate < 4000u || new_rate > 192000u) return;
    atomic_store_explicit(&g_cb_clock_reset, true, memory_order_relaxed);
    if (g_backend != 1 || !g_au) {
        atomic_store_explicit(&g_hw_rate, new_rate, memory_order_relaxed);
        return;
    }

    atomic_store_explicit(&g_audio_reconfiguring, true, memory_order_release);

    AudioOutputUnitStop(g_au);
    AudioUnitUninitialize(g_au);

    atomic_store_explicit(&g_hw_rate, new_rate, memory_order_release);

    AudioStreamBasicDescription f;
    memset(&f, 0, sizeof f);
    f.mSampleRate       = (double)new_rate;
    f.mFormatID         = kAudioFormatLinearPCM;
    f.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = OUT_CH;
    f.mBitsPerChannel   = 16;
    f.mFramesPerPacket  = 1;
    f.mBytesPerFrame    = OUT_CH * sizeof(int16_t);
    f.mBytesPerPacket   = f.mBytesPerFrame;

    AudioUnitSetProperty(g_au, kAudioUnitProperty_StreamFormat,
                         kAudioUnitScope_Input, 0, &f, sizeof f);

    UInt32 mx = FRAMES;
    AudioUnitSetProperty(g_au, kAudioUnitProperty_MaximumFramesPerSlice,
                         kAudioUnitScope_Global, 0, &mx, sizeof mx);

    AudioUnitInitialize(g_au);
    AudioOutputUnitStart(g_au);

    atomic_store_explicit(&g_audio_reconfiguring, false, memory_order_release);
    fprintf(stderr, "  [ses-ios] RemoteIO hatti %u Hz icin basariyla guncellendi\n", new_rate);
}

void audio_ios_restart_subsystem(void){
    atomic_store_explicit(&g_audio_reconfiguring, true, memory_order_release);
    audio_shutdown();
    audio_init();
    atomic_store_explicit(&g_audio_reconfiguring, false, memory_order_release);
}

/* --- Ana Baslatma ve Kapatma Fonksiyonlari --- */

void audio_init(void){
    if (atomic_load_explicit(&g_audio_on, memory_order_relaxed)) return;
    { const char *e = getenv("NOSOUND"); if (e && *e && *e != '0') return; }
    atomic_store_explicit(&g_cb_clock_reset, true, memory_order_relaxed);

    /* Ring buffer baslat */
    audio_ringbuf_init(&g_stream_rb, AUDIO_RING_FRAMES, OUT_CH);

#if TARGET_OS_IPHONE
    if (!audio_setup_ios_session()){
        fprintf(stderr, "ses: iOS AVAudioSession baslatilamadi, ses devre disi\n");
        return;
    }
#endif

    uint32_t hw_rate = (uint32_t)atomic_load_explicit(&g_hw_rate, memory_order_relaxed);
    if (!hw_rate) hw_rate = AUDIO_DEFAULT_RATE;

#if TARGET_OS_IPHONE
    fprintf(stderr, "audio policy: iOS nominal sample rate; explicit DirectSound frequency; cursor-preserving Play\n");
#endif
    { const char *d = getenv("SNDDUMP");
      if (d && *d){ g_dump = fopen(d, "wb");
          fprintf(stderr, "ses: cikis dokumu -> %s (%d Hz s16le %d kanal)\n", d, hw_rate, OUT_CH); } }
    { const char *d = getenv("SNDCSV");
      if (d && *d){ g_csv = fopen(d, "w");
          if (g_csv) fprintf(g_csv, "cb,t,id,rate,bytes,pbyte,wpos,avail,frac,target,trim,ratio,zero\n"); } }

    /* SNDBACKEND=queue ile AudioQueue yedek yoluna donulebilir */
    {
        const char *e = getenv("SNDBACKEND");
        if (!(e && *e == 'q') && au_start(hw_rate)){
            g_backend = 1;
            atomic_store_explicit(&g_audio_on, true, memory_order_release);
            fprintf(stderr, "ses: CoreAudio cikisi acildi (AudioUnit RemoteIO, %d Hz, %d kanal)\n",
                    hw_rate, OUT_CH);
            return;
        }
    }

    AudioStreamBasicDescription f;
    memset(&f, 0, sizeof f);
    f.mSampleRate       = hw_rate;
    f.mFormatID         = kAudioFormatLinearPCM;
    f.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = OUT_CH;
    f.mBitsPerChannel   = 16;
    f.mFramesPerPacket  = 1;
    f.mBytesPerFrame    = OUT_CH * sizeof(int16_t);
    f.mBytesPerPacket   = f.mBytesPerFrame;

    if (AudioQueueNewOutput(&f, cb, NULL, NULL, NULL, 0, &g_q) != noErr){
        fprintf(stderr, "ses: AudioQueue acilamadi\n");
        return;
    }
    for (int i = 0; i < NBUF; i++){
        if (AudioQueueAllocateBuffer(g_q, FRAMES * OUT_CH * sizeof(int16_t), &g_qb[i]) != noErr)
            continue;
        memset(g_qb[i]->mAudioData, 0, FRAMES * OUT_CH * sizeof(int16_t));
        g_qb[i]->mAudioDataByteSize = FRAMES * OUT_CH * sizeof(int16_t);
        AudioQueueEnqueueBuffer(g_q, g_qb[i], 0, NULL);
    }
    AudioQueueSetParameter(g_q, kAudioQueueParam_Volume, 1.0f);
    if (AudioQueueStart(g_q, NULL) != noErr){
        fprintf(stderr, "ses: AudioQueue baslatilamadi\n");
        return;
    }
    g_backend = 2;
    atomic_store_explicit(&g_audio_on, true, memory_order_release);
    fprintf(stderr, "ses: CoreAudio cikisi acildi (AudioQueue, %d Hz, %d kanal)\n",
            hw_rate, OUT_CH);
}

void audio_shutdown(void){
    atomic_store_explicit(&g_cb_clock_reset, true, memory_order_relaxed);
    atomic_store_explicit(&g_audio_on, false, memory_order_release);
    if (g_backend == 1 && g_au){
        AudioOutputUnitStop(g_au);
        AudioUnitUninitialize(g_au);
        AudioComponentInstanceDispose(g_au);
        g_au = NULL;
    } else if (g_backend == 2 && g_q){
        AudioQueueStop(g_q, true);
        AudioQueueDispose(g_q, true);
        g_q = NULL;
    }
    audio_ringbuf_destroy(&g_stream_rb);
    g_backend = 0;
}
#endif /* !NFSU2_AUDIO_CORE_TEST */

void audio_report(void){
    if (g_dump){ fflush(g_dump); fclose(g_dump); g_dump = NULL; }
    if (g_csv){  fflush(g_csv);  fclose(g_csv);  g_csv  = NULL; }
    uint32_t hw_rate = (uint32_t)atomic_load_explicit(&g_hw_rate, memory_order_relaxed);
    if (!hw_rate) hw_rate = AUDIO_DEFAULT_RATE;

    fprintf(stderr, "  ses cikisi        : %s (Donanim: %u Hz), %llu kare karistirildi, "
            "en fazla %llu es zamanli tampon, %d kayitli\n",
            atomic_load_explicit(&g_audio_on, memory_order_relaxed) ? "acik" : "KAPALI",
            hw_rate,
            (unsigned long long)g_mixed_frames,
            (unsigned long long)g_active_peak, g_sndn - 1);
    fprintf(stderr, "  ses Play cagrisi  : %llu durduktan/ilk kez basladi, %llu zaten caliyordu\n",
            (unsigned long long)g_replay, (unsigned long long)g_reissue);
#if TARGET_OS_IPHONE || defined(NFSU2_AUDIO_CORE_TEST)
    fprintf(stderr, "  ses hiz politikasi: iOS nominal ornekleme hizi; pitch yalniz SetFrequency\n");
#else
    fprintf(stderr, "  ses hiz orani    : %.3f (oyun sanal zamani / gercek zaman)%s\n",
            g_rate_ratio, g_sync_mode ? "" : "  [SNDSYNC=0: kapali]");
#endif
    fprintf(stderr, "  ses veri boslugu  : ornek %.1f%% sessiz, %llu geri-cagirmanin cogu bos\n",
            g_out_frames ? 100.0*(double)g_out_zero/(double)g_out_frames : 0.0,
            (unsigned long long)g_gap_cb);
    fprintf(stderr, "  --- ses tamponu basina ---\n");
    for (int i = 1; i < g_sndn; i++){
        SndBuf *b = &g_snd[i];
        if (!b->used) continue;
        fprintf(stderr, "   #%-3d %6u Hz %uch %5.2f sn  Play=%-4llu Stop=%-4llu "
                "Reissue=%-4llu  "
                "yazma=%llu (%llu dolu, %.1f KB)  %s\n",
                i, b->rate, b->chans,
                (double)b->bytes/(double)(b->rate*b->chans*(b->bits/8u)),
                (unsigned long long)b->plays, (unsigned long long)b->stops,
                (unsigned long long)b->reissues,
                (unsigned long long)b->wr_calls, (unsigned long long)b->wr_nonzero,
                b->wr_bytes/1024.0, b->playing ? "CALIYOR" : "durdu");
    }
    {
        uint64_t ov = 0;
        for (int i = 1; i < g_sndn; i++) ov += g_snd[i].over;
        fprintf(stderr, "  ses imlec asimi   : %llu geri-cagirmada oynatma yaziciyi gecti\n",
                (unsigned long long)ov);
    }
    fprintf(stderr, "  ses cikis yolu    : %s, geri cagirma %u kare (%.1f ms kuyruk gecikmesi)\n",
            g_backend == 1 ? "AudioUnit RemoteIO" : g_backend == 2 ? "AudioQueue" : "yok",
            g_backend == 1 ? g_au_slice : (unsigned)(FRAMES*NBUF),
            (g_backend == 1 ? (double)g_au_slice : (double)(FRAMES*NBUF))
                * 1000.0 / (double)hw_rate);
    fprintf(stderr, "  ses kesilmesi     : %llu aclik, en uzun geri-cagirma araligi %.1f ms"
            " (tampon %.1f ms)\n",
            (unsigned long long)g_starved, g_worst_gap_us/1000.0,
            (double)FRAMES*NBUF*1000.0/(double)hw_rate);
    fprintf(stderr, "  gec AudioUnit/Queue callback : %llu (2 dilimden uzun aralik)\n",
            (unsigned long long)g_late_callbacks);
}
