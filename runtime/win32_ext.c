/* Win32 katmani - 2. tur: registry, pencere, senkronizasyon, thread */
#include <strings.h>
#include "cpu.h"
#include "guest_memory.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <setjmp.h>
#include <time.h>
#include <sys/event.h>
#include <sched.h>
#include <TargetConditionals.h>
#include "win32_guest_thread_resources.h"
#include "guest_lock.h"

#ifndef NFS_FULL_RUNTIME
#define NFS_FULL_RUNTIME 0
#endif

#if NFS_FULL_RUNTIME
extern void nfs_game_session_checkpoint(void);
extern long metal_frames(void);
#endif

void dispatch_indirect(CPU*,uint32_t);
void *win32_exit_scope_enter(jmp_buf *target);
void win32_exit_scope_leave(void *previous);
int win32_exit_thread(int code);
int win32_exit_requested(void);
void win32_exit_checkpoint(CPU *c);
void win32_set_last_error(CPU *c, uint32_t value);
void mem_commit(uint32_t,uint32_t);
uint32_t win32_alloc(uint32_t n, int zero);
uint32_t res_find(uint32_t,uint32_t,int);
uint32_t res_data(uint32_t);
uint32_t res_size(uint32_t);

/* ---- nesne tablosu (HANDLE -> tur/durum) ---- */
#define H_BASE 0x00A00000u
typedef enum { OB_NONE=0, OB_EVENT, OB_MUTEX, OB_THREAD, OB_FILE, OB_KEY, OB_SEM } ObKind;
typedef struct {
    ObKind kind; int signaled; int manual;
    pthread_mutex_t mtx; pthread_cond_t cv;
    uint32_t guest_arg, guest_entry, tid;
    uint32_t owner_tid; int rec;      /* MUTEX: sahiplik + ozyineleme */
    pthread_t th; int started; void *pending;
    char name[64];
} Obj;
#define OBJ_MAX 512
static Obj g_obj[OBJ_MAX];
static int g_obj_n = 1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Win32GuestThreadResourcePool g_guest_thread_resources;
typedef struct { uint64_t waits, sets; uint32_t waiter_tid, setter_tid; } WStat;
static WStat g_wstat[64];

/* ---- BUYUK GUEST KILIDI ------------------------------------------------
 * Guest kodu ayni anda tek thread calistirir. Boylece hem lift edilmis kodun
 * paylastigi guest bellek/allocator'lar guvenli olur, hem de komut izi tutarli
 * kalir. Kilit yalnizca bloklayan cagrilarda (Sleep/Wait*) birakilir.      */
static pthread_mutex_t g_guest = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_guest_handoff = PTHREAD_COND_INITIALIZER;
/* Generation/handoff sleepers are protected by g_guest. Ordinary waiters are
 * atomic because they announce themselves before acquiring that mutex. */
static uint64_t g_guest_generation;
static unsigned g_guest_handoff_sleepers;
static uint64_t g_guest_waiters, g_guest_acquisitions;
static uint64_t g_guest_acquire_wait_ns, g_guest_acquire_wait_max_ns;
static uint64_t g_guest_fair_handoffs;
extern CPU *g_cur_cpu;
static _Thread_local CPU *tl_cpu = 0;
static _Thread_local int tl_guest_owned = 0;
static _Thread_local int tl_cs_depth = 0;
static _Thread_local void *tl_obj = 0;   /* bu thread'in nesnesi */
/* Tum guest thread'lerinin CPU durumu: takilma aninda HEPSININ yigin izini
 * dokebilmek icin (tek thread'inki yaniltici olabiliyor). */
#define MAXCPUS 32
CPU *g_all_cpus[MAXCPUS];
uint32_t g_all_tids[MAXCPUS];
int g_all_n = 0;
typedef struct {
    CPU *cpu;
    const char *api;
    uint32_t return_ip;
    uint32_t args[4];
    unsigned nargs, valid_args, args_captured;
} CpuImportDiag;
static CpuImportDiag g_cpu_import_diag[MAXCPUS];
static _Thread_local int tl_diag_slot = -1;

static uint32_t cpu_tid_checked(CPU *c){
    if (!c || !c->fs_base || !g_mem) return 0;
    uint64_t address = (uint64_t)c->fs_base + 0x24u;
    if (address + 4u > GUEST_MEMORY_SIZE) return 0;
    uint32_t addr32 = (uint32_t)address;
    if (!mem_is_committed(addr32) || !mem_is_committed(addr32 + 3u)) return 0;
    return MR32(addr32);
}

static void reg_cpu(CPU *c){
    int free_slot = -1;
    for (int i=0;i<g_all_n;i++){
        if (g_all_cpus[i] == c){ tl_diag_slot = i; return; }
        if (!g_all_cpus[i] && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0){
        g_all_cpus[free_slot] = c;
        g_all_tids[free_slot] = cpu_tid_checked(c);
        memset(&g_cpu_import_diag[free_slot], 0, sizeof(g_cpu_import_diag[free_slot]));
        g_cpu_import_diag[free_slot].cpu = c;
        tl_diag_slot = free_slot;
        return;
    }
    if (g_all_n < MAXCPUS){
        g_all_cpus[g_all_n] = c;
        g_all_tids[g_all_n] = cpu_tid_checked(c);
        memset(&g_cpu_import_diag[g_all_n], 0, sizeof(g_cpu_import_diag[g_all_n]));
        g_cpu_import_diag[g_all_n].cpu = c;
        tl_diag_slot = g_all_n;
        g_all_n++;
    }
}

static int import_args_are_interesting(const char *name){
    if (!name || !name[0]) return 0;
    switch (name[0]){
        case 'W': return !strncmp(name, "Wait", 4);
        case 'E': return !strcmp(name, "EnterCriticalSection");
        case 'L': return !strcmp(name, "LeaveCriticalSection");
        case 'T': return !strcmp(name, "TryEnterCriticalSection");
        case 'S': return !strcmp(name, "Sleep") || !strcmp(name, "SleepEx") ||
                         !strcmp(name, "SetEvent");
        case 'G': return !strcmp(name, "GetFullPathNameA");
        case 'V': return !strcmp(name, "VirtualProtect");
        case 'C': return !strcmp(name, "CreateFileA") ||
                         !strcmp(name, "CreateFileW") ||
                         !strcmp(name, "CreateThread");
        case 'R': return !strcmp(name, "ReleaseMutex") ||
                         !strcmp(name, "ReleaseSemaphore") ||
                         !strcmp(name, "ResetEvent");
        default: return 0;
    }
}

/* Called by the import dispatcher while the guest lock is held, after it
 * removes the stdcall return address. At that point ESP points at argument 0.
 * The API name points into the immutable generated import table. A TLS slot
 * avoids a CPU-table scan per import; only selected APIs take one checked
 * guest-memory span for up to four argument values. */
void guest_note_import(CPU *c, const char *name, unsigned nargs){
    int slot = tl_diag_slot;
    if (!c || tl_cpu != c || slot < 0 || slot >= MAXCPUS ||
        g_all_cpus[slot] != c || g_cpu_import_diag[slot].cpu != c) return;

    CpuImportDiag *diag = &g_cpu_import_diag[slot];
    diag->api = name ? name : "?";
    diag->return_ip = c->eip;
    diag->nargs = nargs;
    diag->valid_args = 0;
    diag->args_captured = 0;

    if (import_args_are_interesting(name)){
        unsigned count = nargs < 4u ? nargs : 4u;
        diag->args_captured = 1;
        uint64_t bytes = (uint64_t)count * sizeof(uint32_t);
        uint64_t end = (uint64_t)c->esp + bytes;
        if (count && g_mem && end <= GUEST_MEMORY_SIZE &&
            mem_is_committed(c->esp) && mem_is_committed((uint32_t)(end - 1u))){
            memcpy(diag->args, g_mem + c->esp, (size_t)bytes);
            diag->valid_args = (1u << count) - 1u;
        }
    }
}

static void unreg_cpu(CPU *c){
    if (!c) return;
    pthread_mutex_lock(&g_guest);
    for (int i=0;i<g_all_n;i++) if (g_all_cpus[i] == c){
        g_all_cpus[i] = 0;
        g_all_tids[i] = 0;
        memset(&g_cpu_import_diag[i], 0, sizeof(g_cpu_import_diag[i]));
    }
    if (g_cur_cpu == c) g_cur_cpu = 0;
    if (tl_cpu == c) tl_cpu = 0;
    tl_diag_slot = -1;
    pthread_mutex_unlock(&g_guest);
}

static uint64_t diagnostic_wall_ms(void){
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || ts.tv_sec < 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

#if NFS_FULL_RUNTIME
#define STALL_TEXT_BASE 0x00401000u
#define STALL_TEXT_SIZE 0x00381af1u
#define STALL_MAX_STACK_WORDS 32u
#define STALL_MAX_CANDIDATES 8u

static uint64_t diagnostic_mono_us(void){
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || ts.tv_sec < 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* Bounded stack inspection: read each word through the checked guest-memory
 * accessor, keep only values inside the guest code range, and never read code
 * bytes or dereference candidate addresses. These are candidates, not a full
 * unwound trace (x86 stack arguments may also resemble code addresses). */
static void print_stack_candidates(CPU *c){
    unsigned found = 0;
    fprintf(stderr, " stack-candidates=");
    for (unsigned word=0; c && word<STALL_MAX_STACK_WORDS &&
         found<STALL_MAX_CANDIDATES; ++word){
        uint64_t address = (uint64_t)c->esp + (uint64_t)word * sizeof(uint32_t);
        if (address > UINT32_MAX - 3u) break;
        const uint32_t *p = (const uint32_t *)guest_memory_pointer(
            (uint32_t)address, sizeof(*p), 0);
        if (!p) continue;
        uint32_t value;
        memcpy(&value, p, sizeof(value));
        if (value >= STALL_TEXT_BASE &&
            (uint64_t)value < (uint64_t)STALL_TEXT_BASE + STALL_TEXT_SIZE){
            fprintf(stderr, "%s%08x", found ? "," : "", value);
            found++;
        }
    }
    if (!found) fprintf(stderr, "none");
}

static void dump_stall_cpus(long frames, uint64_t stalled_us, unsigned dump_no){
    uint64_t wall_ms = diagnostic_wall_ms();
    fprintf(stderr, "[nfsu2-stall] wall_ms=%llu frames=%ld stalled_ms=%llu dump=%u/4 cpus=%d\n",
            (unsigned long long)wall_ms, frames,
            (unsigned long long)(stalled_us / 1000ull), dump_no, g_all_n);
    for (int i=0; i<g_all_n; ++i){
        CPU *c = g_all_cpus[i];
        if (!c) continue;
        CpuImportDiag *diag = &g_cpu_import_diag[i];
        uint32_t tid = cpu_tid_checked(c);
        if (!tid) tid = g_all_tids[i];
        fprintf(stderr,
                "  cpu[%d] tid=%u eip=%08x esp=%08x ebp=%08x last=%s ra=%08x nargs=%u args=",
                i, tid, c->eip, c->esp, c->ebp,
                (diag->cpu == c && diag->api) ? diag->api : "<none>",
                (diag->cpu == c) ? diag->return_ip : 0u,
                (diag->cpu == c) ? diag->nargs : 0u);
        if (diag->cpu == c){
            if (!diag->args_captured){
                fputs("<not-captured>", stderr);
            } else {
                unsigned count = diag->nargs < 4u ? diag->nargs : 4u;
                if (count && !diag->valid_args){
                    fputs("<invalid>", stderr);
                } else {
                    for (unsigned a=0; a<count; ++a){
                        if (a) fputc(',', stderr);
                        if (diag->valid_args & (1u << a))
                            fprintf(stderr, "%08x", diag->args[a]);
                    }
                }
            }
        } else {
            fputs("<unavailable>", stderr);
        }
        print_stack_candidates(c);
        fputc('\n', stderr);
    }
    fflush(stderr);
}

/* Called only from the existing 400-WaitForSingleObject diagnostic point,
 * with the guest lock held. The Metal frame counter is atomic; CPU snapshots
 * therefore never run on the UI/render timer thread. A progress change resets
 * the episode and its four-dump cap. */
static void stall_checkpoint(void){
    static long observed_frames = -1;
    static uint64_t last_progress_us = 0;
    static uint64_t next_dump_us = 0;
    static unsigned dumps = 0;
    uint64_t now_us = diagnostic_mono_us();
    long frames = metal_frames();
    if (frames <= 0){
        observed_frames = -1;
        last_progress_us = 0;
        next_dump_us = 0;
        dumps = 0;
        return;
    }
    if (observed_frames != frames || !last_progress_us){
        observed_frames = frames;
        last_progress_us = now_us;
        next_dump_us = now_us + 3000000ull;
        dumps = 0;
        return;
    }
    if (dumps < 4u && now_us >= next_dump_us){
        ++dumps;
        dump_stall_cpus(frames, now_us - last_progress_us, dumps);
        next_dump_us = now_us + 3000000ull;
    }
}
#endif

/* --- buyuk kilidin (GIL) TUTMA SURESI olcumu --------------------------
 * Guest thread'i Win32 cagrisi yapmadigi surece GIL'i birakmaz; uzun bir
 * hesaplama (ornek: ses akisi cozme) diger thread'leri aclikta birakir.
 * Ses her 30 sn'de bir ~50 ms sessizlik yaziyor -- suclunun bu olup
 * olmadigini olcuyoruz.  GILLOG=<ms> esigi asan her tutusu bildirir. */
static uint64_t gil_now_us(void){
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec*1000000ull + (uint64_t)ts.tv_nsec/1000ull;
}
static _Thread_local uint64_t tl_gil_t0 = 0;
uint64_t g_gil_max_us = 0, g_gil_long = 0;
static int gil_thr_us(void){
    static int v = -1;
    if (v < 0){ const char *e = getenv("GILLOG"); v = (e && *e) ? atoi(e)*1000 : 0; }
    return v;
}
static void gil_took(void){ tl_gil_t0 = gil_now_us(); }
static void gil_gave(void){
    if (!tl_gil_t0) return;
    uint64_t now = gil_now_us();
    uint64_t d = now >= tl_gil_t0 ? now - tl_gil_t0 : 0;
    tl_gil_t0 = 0;
    if (d > g_gil_max_us) g_gil_max_us = d;
    int thr = gil_thr_us();
    if (thr && d > (uint64_t)thr){
        g_gil_long++;
        static int shown = 0;
        if (shown < 80){ shown++;
            double audio_time(void);
            const char *api = tl_diag_slot >= 0 && tl_diag_slot < MAXCPUS
                ? g_cpu_import_diag[tl_diag_slot].api : NULL;
            fprintf(stderr, "  [GIL] t=%.3f sn  kilit %.1f ms tutuldu  guest eip=%08x "
                    "wall_ms=%llu tid=%u cs_depth=%d last_import=%s\n",
                    audio_time(), d/1000.0, tl_cpu ? tl_cpu->eip : 0u,
                    (unsigned long long)diagnostic_wall_ms(), cpu_tid_checked(tl_cpu),
                    tl_cs_depth, api ? api : "none");
        }
    }
}
static uint64_t g_host_waits, g_host_wait_ns, g_max_host_wait_ns;
static _Thread_local uint64_t tl_host_wait_start_us;

/* Called only while owning g_guest, after a real execution acquisition.
 * unreg_cpu's maintenance lock deliberately does not advance this generation. */
static void guest_record_acquisition(uint64_t started_us){
    uint64_t now = gil_now_us();
    uint64_t ns = started_us && now >= started_us ? (now - started_us) * 1000ull : 0;
    __atomic_fetch_add(&g_guest_acquisitions, 1ull, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_guest_acquire_wait_ns, ns, __ATOMIC_RELAXED);
    uint64_t maximum = __atomic_load_n(&g_guest_acquire_wait_max_ns, __ATOMIC_RELAXED);
    if (ns > maximum)
        __atomic_store_n(&g_guest_acquire_wait_max_ns, ns, __ATOMIC_RELAXED);
    ++g_guest_generation;
    if (g_guest_handoff_sleepers) pthread_cond_broadcast(&g_guest_handoff);
}

static void guest_acquire_mutex(void){
    uint64_t started = gil_now_us();
    __atomic_fetch_add(&g_guest_waiters, 1ull, __ATOMIC_RELAXED);
    pthread_mutex_lock(&g_guest);
    __atomic_fetch_sub(&g_guest_waiters, 1ull, __ATOMIC_RELAXED);
    tl_guest_owned = 1;
    guest_record_acquisition(started);
}

void guest_lock(CPU *c){
#if NFS_FULL_RUNTIME
    nfs_game_session_checkpoint();
#endif
    guest_acquire_mutex();
    tl_cpu=c; g_cur_cpu=c; reg_cpu(c); gil_took();
}
void guest_unlock(void){
    gil_gave(); tl_guest_owned=0; pthread_mutex_unlock(&g_guest);
}
static void guest_yield_begin(void){
    gil_gave(); tl_guest_owned=0; pthread_mutex_unlock(&g_guest);
}
static void guest_yield_end(void){
    guest_acquire_mutex();
    g_cur_cpu = tl_cpu; gil_took();
}

int guest_host_wait_begin(void){
    if (!tl_guest_owned) return 0;
    guest_yield_begin();
    tl_host_wait_start_us = gil_now_us();
    return 1;
}

void guest_host_wait_end(int released){
    if (!released) return;
    uint64_t started = tl_host_wait_start_us;
    guest_yield_end();
    uint64_t now = gil_now_us();
    uint64_t ns = started && now >= started ? (now - started) * 1000ull : 0;
    __atomic_fetch_add(&g_host_waits, 1ull, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_host_wait_ns, ns, __ATOMIC_RELAXED);
    uint64_t maximum = __atomic_load_n(&g_max_host_wait_ns, __ATOMIC_RELAXED);
    while (maximum < ns && !__atomic_compare_exchange_n(
            &g_max_host_wait_ns, &maximum, ns, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    tl_host_wait_start_us = 0;
}

void guest_lock_snapshot(GuestLockSnapshot *snapshot){
    if (!snapshot) return;
    snapshot->host_waits = __atomic_load_n(&g_host_waits, __ATOMIC_RELAXED);
    snapshot->host_wait_ns = __atomic_load_n(&g_host_wait_ns, __ATOMIC_RELAXED);
    snapshot->max_host_wait_ns = __atomic_load_n(&g_max_host_wait_ns, __ATOMIC_RELAXED);
    snapshot->acquisition_wait_ns = __atomic_load_n(&g_guest_acquire_wait_ns, __ATOMIC_RELAXED);
    snapshot->acquisition_wait_max_ns = __atomic_load_n(&g_guest_acquire_wait_max_ns, __ATOMIC_RELAXED);
    snapshot->acquisitions = __atomic_load_n(&g_guest_acquisitions, __ATOMIC_RELAXED);
    snapshot->queued_waiters = __atomic_load_n(&g_guest_waiters, __ATOMIC_RELAXED);
    snapshot->fair_handoffs = __atomic_load_n(&g_guest_fair_handoffs, __ATOMIC_RELAXED);
}

#if NFS_FULL_RUNTIME && TARGET_OS_IPHONE
static int guest_handoff_deadline(struct timespec *deadline){
    /* g_guest_handoff uses the default CLOCK_REALTIME condvar clock. */
    if (clock_gettime(CLOCK_REALTIME, deadline) != 0) return 0;
    deadline->tv_nsec += 2000000L;
    if (deadline->tv_nsec >= 1000000000L){
        ++deadline->tv_sec; deadline->tv_nsec -= 1000000000L;
    }
    return 1;
}

static void guest_poll_handoff(void){
    /* Cadence must survive ordinary unlock/relock and lifecycle checkpoints.
     * gil_took() resets the ownership-duration clock at each acquisition. A
     * fast 1024-import loop can therefore keep that clock below 2 ms forever,
     * even while peers remain queued. The first eligible queued poll yields
     * promptly; subsequent attempts are throttled from their own completion. */
    static _Thread_local uint64_t last_handoff_us;
    if (!tl_guest_owned || tl_cs_depth ||
        !__atomic_load_n(&g_guest_waiters, __ATOMIC_RELAXED)) return;
    uint64_t now = gil_now_us();
    if (!now || (last_handoff_us && now >= last_handoff_us &&
                 now - last_handoff_us < 2000ull)) return;
    struct timespec deadline;
    if (!guest_handoff_deadline(&deadline)) return;
    uint64_t generation = g_guest_generation;
    uint64_t started = now;
    ++g_guest_handoff_sleepers;
    /* Stay visible while cond-wait is reacquiring g_guest, so the new owner
     * also has a reason to hand execution back at its next safe boundary. */
    __atomic_fetch_add(&g_guest_waiters, 1ull, __ATOMIC_RELAXED);
    gil_gave(); tl_guest_owned = 0;
    while (g_guest_generation == generation &&
           __atomic_load_n(&g_guest_waiters, __ATOMIC_RELAXED) > 1ull &&
           !win32_exit_requested()){
        int rc = pthread_cond_timedwait(&g_guest_handoff, &g_guest, &deadline);
        if (g_guest_generation != generation) break;
        /* Keep the original deadline across spurious wakes. A signaled waiter
         * may be OS-descheduled; do not renew forever behind its live count.
         * Mutex reacquisition itself remains subject to host scheduling. */
        if (rc != 0) break;
    }
    int transferred = g_guest_generation != generation;
    __atomic_fetch_sub(&g_guest_waiters, 1ull, __ATOMIC_RELAXED);
    --g_guest_handoff_sleepers;
    tl_guest_owned = 1; g_cur_cpu = tl_cpu;
    /* The cond-wait's self-reacquisition advances generation only now, after
     * the wait predicate has been decided. It cannot satisfy its own handoff. */
    guest_record_acquisition(started);
    gil_took();
    last_handoff_us = gil_now_us();
    if (transferred)
        __atomic_fetch_add(&g_guest_fair_handoffs, 1ull, __ATOMIC_RELAXED);
}
#endif

void guest_frame_checkpoint(void){
#if NFS_FULL_RUNTIME && TARGET_OS_IPHONE
    guest_poll_handoff();
#endif
}
/* Buyuk kilidi birakarak uyu (win32.c: oyunun kare bekleme dongusu). */
uint64_t g_sleep_late_ns, g_sleep_gil_ns, g_sleep_n;    /* tani: uyku gecikmesi / kilit bekleme */
/* nanosleep macOS'ta zamanlayici birlestirme payiyla uyaniyor: oyunda 3 ms
 * istekte ortalama 447 us gec (ayri olcum: medyan ~770 us, QoS fark etmiyor).
 * kevent zamanlayicisi NOTE_CRITICAL ile birlestirilmez: medyan ~45 us. */
static void sleep_precise_ns(long ns){
    static _Thread_local int kq = -1;
    if (kq < 0) kq = kqueue();
    if (kq >= 0){
        struct kevent ev, out;
        EV_SET(&ev, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_NSECONDS | NOTE_CRITICAL, ns, 0);
        if (kevent(kq, &ev, 1, &out, 1, NULL) == 1) return;
    }
    struct timespec ts = { (time_t)(ns / 1000000000L), ns % 1000000000L };
    nanosleep(&ts, 0);
}
void guest_sleep_ns(long ns){
    struct timespec a, b, d;
    guest_yield_begin();
    clock_gettime(CLOCK_MONOTONIC, &a); sleep_precise_ns(ns); clock_gettime(CLOCK_MONOTONIC, &b);
    guest_yield_end();
    clock_gettime(CLOCK_MONOTONIC, &d);
    int64_t sl = (int64_t)(b.tv_sec - a.tv_sec) * 1000000000LL + (b.tv_nsec - a.tv_nsec) - ns;
    int64_t gl = (int64_t)(d.tv_sec - b.tv_sec) * 1000000000LL + (d.tv_nsec - b.tv_nsec);
    g_sleep_late_ns += sl > 0 ? (uint64_t)sl : 0; g_sleep_gil_ns += (uint64_t)gl; g_sleep_n++;
}

/* --- kare hizi sabitleme (60 Hz) ------------------------------------
 * Sanal saat kare basina TAM 16.667 ms ilerliyor, yani oyunun hizi =
 * (gercek fps / 60).  Emulator 117 fps yapinca oyun %195 hizda
 * kosuyordu.  Kareyi 60 Hz'e sabitlemek hizi FPS'ten bagimsiz kilar.
 * Uyku sirasinda buyuk kilit BIRAKILIR, yoksa isci thread'ler durur. */
/* TIMELOG=1: saniyede bir sanal/gercek zaman orani ve kare sayisi.
 * "Oyun ara sahnelerde gereksiz hizlaniyor" sikayetini olcmek icin --
 * oran 1.0'in uzerine cikiyorsa oyun gercekten hizli kosuyor demektir. */
static void timelog_tick(void){
    static int on = -1;
    if (on < 0){ const char *e = getenv("TIMELOG"); on = (e && *e && *e != '0'); }
    if (!on) return;
    uint64_t host_us(void);
    static uint64_t lr = 0, lv = 0; static long lf = 0;
    long metal_frames(void);
    uint64_t r = gil_now_us(), v = host_us();
    long f = metal_frames();
    if (!lr){ lr = r; lv = v; lf = f; return; }
    if (r - lr < 1000000ull) return;
    fprintf(stderr, "  [zaman] gercek %.2f sn, sanal %.2f sn, oran %.3f, %ld kare (%.1f fps)\n",
            (r-lr)/1e6, (v-lv)/1e6, (double)(v-lv)/(double)(r-lr),
            f-lf, (f-lf)*1e6/(double)(r-lr));
    lr = r; lv = v; lf = f;
}

void frame_pace(void){
    timelog_tick();
    /* VARSAYILAN: FREN YOK.  Yukaridaki 60 Hz gerekcesi ARTIK GECERSIZ --
     * sanal saat VCLOCK=real'e gectiginden oyun hizi gercek fps'ten
     * bagimsiz (olculdu: 63.7 fps'te de 99 fps'te de sanal/gercek =
     * 0.999).  Fren acikken yaris karesinin %61'i nanosleep'ti; kapaninca
     * yaris 63.7 -> 79.4 fps, menuler 120 fps (ekran tazeleme siniri).
     * PACE60=1 eski davranisi geri getirir.  Kacak fps'i vsync zaten
     * kesiyor; sanal saat kare basi artisi 4 ms'de (250 fps) tabanli. */
    static int off = -1;
    if (off < 0){ const char *e = getenv("PACE60");
                  off = !(e && *e && *e != '0'); }
    if (off) return;
    static uint64_t deadline = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec*1000000ull + (uint64_t)ts.tv_nsec/1000ull;
    if (!deadline || now > deadline + 200000ull) deadline = now;   /* cok geride: sifirla */
    deadline += 16667ull;
    if (now < deadline){
        uint64_t d = deadline - now;
        struct timespec sl = { (time_t)(d/1000000ull), (long)((d%1000000ull)*1000ull) };
        guest_yield_begin();
        nanosleep(&sl, 0);
        guest_yield_end();
    }
}

/* Adil GIL: guest kodu bloklamayan Win32 cagrilarinda da donebilir
 * (ornek: EnterCriticalSection ile donen bekleme dongusu). Periyodik
 * olarak kilidi birakip diger guest thread'lerine sira veriyoruz.     */
/* ---- GERCEK kritik bolumler ------------------------------------------
 * Onceden EnterCriticalSection/LeaveCriticalSection etkisiz stub'di.  Tek
 * buyuk kilit (GIL) cogu zaman ortuyordu AMA guest_fair_yield her 1024
 * Win32 cagrisinda GIL'i birakiyor: bir thread kritik bolumun ICINDEYKEN
 * kilidi kaybedebiliyor ve baska bir thread ayni bolume girebiliyordu.
 * Oyunun yukleyici/ses/video kuyruklari boyle sessizce bozulur.
 *
 * Kilidi beklerken GIL BIRAKILMALI, yoksa kilitlenme olur.              */
#define MAXCS 512
static struct { uint32_t key; pthread_mutex_t m; int used; } g_cs[MAXCS];
static pthread_mutex_t g_cs_tab = PTHREAD_MUTEX_INITIALIZER;
uint64_t g_cs_enter = 0, g_cs_contended = 0;

static int cs_disabled(void){
    static int v = -1;
    if (v < 0){ const char *e = getenv("NOCS"); v = (e && *e && *e != '0') ? 1 : 0; }
    return v;
}
static pthread_mutex_t *cs_for(uint32_t key){
    if (cs_disabled()) return 0;          /* eski davranis: etkisiz stub */
    static _Thread_local uint32_t tl_last_cs_key = 0;
    static _Thread_local pthread_mutex_t *tl_last_cs_mtx = 0;
    if (key && key == tl_last_cs_key && tl_last_cs_mtx) return tl_last_cs_mtx;
    pthread_mutex_lock(&g_cs_tab);
    unsigned h = (key >> 2) % MAXCS;
    for (unsigned i = 0; i < MAXCS; i++){
        unsigned j = (h + i) % MAXCS;
        if (g_cs[j].used && g_cs[j].key == key){
            pthread_mutex_t *m = &g_cs[j].m;
            pthread_mutex_unlock(&g_cs_tab);
            tl_last_cs_key = key; tl_last_cs_mtx = m;
            return m;
        }
        if (!g_cs[j].used){
            pthread_mutexattr_t a;
            pthread_mutexattr_init(&a);
            pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&g_cs[j].m, &a);
            pthread_mutexattr_destroy(&a);
            g_cs[j].key = key; g_cs[j].used = 1;
            pthread_mutex_t *m = &g_cs[j].m;
            pthread_mutex_unlock(&g_cs_tab);
            tl_last_cs_key = key; tl_last_cs_mtx = m;
            return m;
        }
    }
    pthread_mutex_unlock(&g_cs_tab);
    return 0;                      /* tablo doldu: eski davranisa don */
}

uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
static uint32_t g_wndproc = 0;
static uint32_t g_hwnd    = 0x00B00001u;
#define WMQ 64
static struct { uint32_t msg, wp, lp; } g_wmq[WMQ];
static int g_wmq_head = 0, g_wmq_tail = 0;
static int g_wm_activated = 0;
uint64_t g_wm_sent = 0, g_wm_dispatched = 0;

static void wm_push(uint32_t m, uint32_t wp, uint32_t lp){
    int nx = (g_wmq_tail + 1) % WMQ;
    if (nx == g_wmq_head) return;
    g_wmq[g_wmq_tail].msg = m; g_wmq[g_wmq_tail].wp = wp; g_wmq[g_wmq_tail].lp = lp;
    g_wmq_tail = nx;
}
static int wm_pop(uint32_t *m, uint32_t *wp, uint32_t *lp){
    if (g_wmq_head == g_wmq_tail) return 0;
    *m = g_wmq[g_wmq_head].msg; *wp = g_wmq[g_wmq_head].wp; *lp = g_wmq[g_wmq_head].lp;
    g_wmq_head = (g_wmq_head + 1) % WMQ;
    return 1;
}

void metal_get_cursor(int *x, int *y);
void metal_fill_mouse(uint8_t *out, unsigned cb);
void metal_fill_keyboard(uint8_t *out, unsigned cb);

static int win32_vk_is_down(uint32_t vk, const uint8_t *ks, const uint8_t *ms){
    switch (vk){
        case 0x01u: return ms[12] != 0;                              /* VK_LBUTTON */
        case 0x02u: return ms[13] != 0;                              /* VK_RBUTTON */
        case 0x08u: return ks[0x0E] != 0;                            /* VK_BACK */
        case 0x09u: return ks[0x0F] != 0;                            /* VK_TAB */
        case 0x0Du: return ks[0x1C] != 0 || ks[0x9C] != 0;           /* VK_RETURN */
        case 0x10u: return ks[0x2A] != 0 || ks[0x36] != 0;           /* VK_SHIFT */
        case 0x11u: return ks[0x1D] != 0 || ks[0x9D] != 0;           /* VK_CONTROL */
        case 0x12u: return ks[0x38] != 0 || ks[0xB8] != 0;           /* VK_MENU */
        case 0x1Bu: return ks[0x01] != 0;                            /* VK_ESCAPE */
        case 0x20u: return ks[0x39] != 0;                            /* VK_SPACE */
        case 0x25u: return ks[0xCB] != 0;                            /* VK_LEFT */
        case 0x26u: return ks[0xC8] != 0;                            /* VK_UP */
        case 0x27u: return ks[0xCD] != 0;                            /* VK_RIGHT */
        case 0x28u: return ks[0xD0] != 0;                            /* VK_DOWN */
        case 0xA0u: return ks[0x2A] != 0;                            /* VK_LSHIFT */
        case 0xA1u: return ks[0x36] != 0;                            /* VK_RSHIFT */
        case 0xA2u: return ks[0x1D] != 0;                            /* VK_LCONTROL */
        case 0xA3u: return ks[0x9D] != 0;                            /* VK_RCONTROL */
        default: break;
    }
    return 0;
}

static void wm_poll_input(void){
    if (!g_wm_activated) return;
    static int last_mx = -1, last_my = -1, last_ldown = 0;
    static uint8_t last_key_down[8] = {0};
    int mx = 0, my = 0;
    uint8_t ms[20] = {0};
    uint8_t ks[256] = {0};
    metal_get_cursor(&mx, &my);
    metal_fill_mouse(ms, sizeof(ms));
    metal_fill_keyboard(ks, sizeof(ks));

    int ldown = (ms[12] != 0);
    uint32_t lp = ((uint32_t)(uint16_t)my << 16) | (uint32_t)(uint16_t)mx;
    uint32_t wp_mouse = ldown ? 1u : 0u; /* MK_LBUTTON = 0x0001 */

    if (last_mx < 0){
        last_mx = mx;
        last_my = my;
    } else if (mx != last_mx || my != last_my){
        last_mx = mx;
        last_my = my;
        wm_push(0x0200u, wp_mouse, lp); /* WM_MOUSEMOVE */
    }
    if (ldown != last_ldown){
        last_ldown = ldown;
        wm_push(ldown ? 0x0201u : 0x0202u, wp_mouse, lp); /* WM_LBUTTONDOWN / WM_LBUTTONUP */
    }

    static const struct { uint8_t dik; uint8_t vk; } k_nav[8] = {
        { 0x1Cu, 0x0Du }, /* DIK_RETURN -> VK_RETURN */
        { 0x01u, 0x1Bu }, /* DIK_ESCAPE -> VK_ESCAPE */
        { 0x0Eu, 0x08u }, /* DIK_BACK   -> VK_BACK   */
        { 0x39u, 0x20u }, /* DIK_SPACE  -> VK_SPACE  */
        { 0xC8u, 0x26u }, /* DIK_UP     -> VK_UP     */
        { 0xD0u, 0x28u }, /* DIK_DOWN   -> VK_DOWN   */
        { 0xCBu, 0x25u }, /* DIK_LEFT   -> VK_LEFT   */
        { 0xCDu, 0x27u }, /* DIK_RIGHT  -> VK_RIGHT  */
    };
    for (unsigned i = 0; i < 8u; i++){
        uint8_t down = (ks[k_nav[i].dik] & 0x80u) ? 1u : 0u;
        if (down != last_key_down[i]){
            last_key_down[i] = down;
            uint32_t klp = ((uint32_t)k_nav[i].dik << 16) | (down ? 1u : 0xC0000001u);
            wm_push(down ? 0x0100u : 0x0101u, (uint32_t)k_nav[i].vk, klp); /* WM_KEYDOWN / WM_KEYUP */
        }
    }
}

static uint32_t cur_tid(CPU *c){
    return (c && c->fs_base && mem_is_committed(c->fs_base+0x24u))
           ? MR32(c->fs_base+0x24u) : 0u;
}

void guest_fair_yield(void){
#if NFS_FULL_RUNTIME && TARGET_OS_IPHONE
    static _Thread_local uint32_t polls = 0;
    ++polls;
    /* Keep the v25 host-scheduler yield as well as lifecycle progress at this
     * boundary. Handoff cadence above is independent of this reacquisition. */
    if ((polls & 0x3FFu) == 0 && tl_guest_owned && !tl_cs_depth){
        guest_yield_begin();
        sched_yield();
        nfs_game_session_checkpoint();
        guest_yield_end();
    }
    if ((polls & 0x7Fu) == 0) guest_poll_handoff();
#else
    if (tl_cs_depth) return;          /* kritik bolum icinde sira verme */
    static _Thread_local uint32_t n = 0;
    if ((++n & 0x3FFu) != 0) return;          /* her 1024 cagrida bir */
    guest_yield_begin();
    sched_yield();
    /* DIKKAT: burada her seferinde nanosleep(50us) vardi.  macOS'ta 50 us
     * istenen bir uyku gercekte ~1-2 ms surer (zamanlayici kuantumu +
     * timer coalescing).  Yarisdan onceki ara sahnede guest bir Win32
     * fonksiyonunu kare basina ~110 bin kez cagiriyor; bu da kare basina
     * ~110 uyku = ~165 ms demekti.  Olculdu: guest is parcaciginin %70'i
     * __semwait_signal icinde, yani hesap degil UYKU.  Kare suresi 235 ms
     * (4.6 fps).  sched_yield adilligi zaten sagliyor; uykuyu seyreltiyoruz
     * (yalnizca diger is parcaciklarinin ilerlemesini garantilemek icin). */
    if ((n & 0x7FFFu) == 0){
        struct timespec ts = {0, 50000L}; nanosleep(&ts, 0);
    }
#if NFS_FULL_RUNTIME
    nfs_game_session_checkpoint();
#endif
    guest_yield_end();
#endif
}

static uint32_t obj_new(ObKind k){
    pthread_mutex_lock(&g_lock);
    int i = g_obj_n < OBJ_MAX ? g_obj_n++ : 0;
    pthread_mutex_unlock(&g_lock);
    if (!i) return 0;
    Obj *o=&g_obj[i]; memset(o,0,sizeof *o); o->kind=k;
    pthread_mutex_init(&o->mtx,0); pthread_cond_init(&o->cv,0);
    return H_BASE + (uint32_t)i;
}
/* fileio.c'nin overlapped tamamlanma olayini sinyallemesi icin. */
void win32_signal_event(uint32_t h);

static Obj *obj_get(uint32_t h){
    if (h < H_BASE || h >= H_BASE+OBJ_MAX) return 0;
    uint32_t i = h - H_BASE;
    return g_obj[i].kind ? &g_obj[i] : 0;
}

/* WaitForSingleObject uses a realtime-clock condition variable. Keep one
 * absolute deadline for finite waits, but cap each sleep so process exit is
 * still observed during INFINITE waits (and long finite waits). */
#define WIN32_WAIT_INFINITE 0xFFFFFFFFu
#define WIN32_WAIT_EXIT_POLL_MS 2000u
#define WIN32_WAIT_FAILED 0xFFFFFFFFu
#define WIN32_ERROR_GEN_FAILURE 31u

static int win32_wait_timespec_cmp(const struct timespec *a,
                                   const struct timespec *b){
    if (a->tv_sec != b->tv_sec) return a->tv_sec < b->tv_sec ? -1 : 1;
    if (a->tv_nsec != b->tv_nsec) return a->tv_nsec < b->tv_nsec ? -1 : 1;
    return 0;
}

static int win32_wait_deadline_after_ms(struct timespec *deadline,
                                        uint32_t ms){
    if (!deadline || clock_gettime(CLOCK_REALTIME, deadline) != 0) return 0;
    uint32_t seconds = ms / 1000u;
    long nanoseconds = deadline->tv_nsec + (long)(ms % 1000u) * 1000000L;
    if (nanoseconds >= 1000000000L){
        ++seconds;
        nanoseconds -= 1000000000L;
    }
    /* ms is 32-bit, so its seconds conversion above is also 32-bit. Guard
     * the eventual time_t addition on legacy narrower time_t targets. */
    if (sizeof(time_t) <= sizeof(uint64_t)){
        const int is_signed = (time_t)-1 < (time_t)0;
        const unsigned width = (unsigned)(sizeof(time_t) * CHAR_BIT);
        const unsigned value_bits = width - (is_signed ? 1u : 0u);
        uint64_t max_seconds = UINT64_MAX;
        if (value_bits < 64u)
            max_seconds = (UINT64_C(1) << value_bits) - 1u;
        const uint64_t now_seconds = (uint64_t)deadline->tv_sec;
        if (now_seconds > max_seconds ||
            (uint64_t)seconds > max_seconds - now_seconds) return 0;
    }
    deadline->tv_sec += (time_t)seconds;
    deadline->tv_nsec = nanoseconds;
    return 1;
}

static int win32_wait_deadline_expired(const struct timespec *deadline){
    struct timespec now;
    if (!deadline || clock_gettime(CLOCK_REALTIME, &now) != 0) return -1;
    return win32_wait_timespec_cmp(&now, deadline) >= 0;
}

static int win32_wait_next_chunk(struct timespec *wake_deadline,
                                 const struct timespec *deadline,
                                 int finite){
    if (!win32_wait_deadline_after_ms(wake_deadline,
                                     WIN32_WAIT_EXIT_POLL_MS)) return 0;
    if (finite && win32_wait_timespec_cmp(deadline, wake_deadline) < 0)
        *wake_deadline = *deadline;
    return 1;
}

static int win32_wait_cond_step(Obj *o, const struct timespec *deadline,
                               int finite){
    struct timespec wake_deadline;
    if (!win32_wait_next_chunk(&wake_deadline, deadline, finite)) return -1;
    int rc = pthread_cond_timedwait(&o->cv, &o->mtx, &wake_deadline);
    if (rc != 0 && rc != ETIMEDOUT) return -1;
    return 0;
}

static int win32_wait_object_signaled(const Obj *o){
    return o->kind == OB_SEM ? o->signaled > 0 : o->signaled != 0;
}

static void win32_wait_object_consume(Obj *o){
    if (o->kind == OB_SEM) --o->signaled;
    else if (o->kind != OB_THREAD && !o->manual) o->signaled = 0;
}

/* ---- guest thread ---- */
uint64_t g_threads_started = 0, g_threads_exited = 0;

typedef struct {
    uint32_t entry, arg;
    Obj *ob;
    Win32GuestThreadReservation resources;
} TArg;

typedef struct {
    Win32GuestThreadReservation resources;
} TimerThreadStart;

/* g_lock protects pool operations and ownership transitions for suspended
 * TArgs. Started workers release only after CPU cleanup/unregistration. */
static void release_failed_start_locked(Win32GuestThreadReservation *resources){
    if (resources && resources->active)
        (void)win32_guest_thread_resources_release_failed_start(
            &g_guest_thread_resources, resources);
}

static void release_quiesced_locked(Win32GuestThreadReservation *resources){
    if (resources && resources->active)
        (void)win32_guest_thread_resources_release_quiesced(
            &g_guest_thread_resources, resources);
}

static void prepare_guest_thread_resources(
    const Win32GuestThreadReservation *resources){
    mem_commit(resources->teb, WIN32_GUEST_THREAD_TEB_SIZE);
    /* TEBs are 8 KiB while commits round to 16 KiB; clear exactly this TEB so
     * its neighbor's half of a committed granule is never overwritten. */
    memset(g_mem + resources->teb, 0, WIN32_GUEST_THREAD_TEB_SIZE);
    mem_commit(resources->stack_low, WIN32_GUEST_THREAD_STACK_SIZE);
    /* A newly created Windows stack is demand-zeroed. Reused guest stack slots
     * must expose the same initial contents to the next worker. */
    memset(g_mem + resources->stack_low, 0, WIN32_GUEST_THREAD_STACK_SIZE);
}

static void discard_failed_thread_start(Obj *o, TArg *t){
    pthread_mutex_lock(&g_lock);
    if (o){
        if (o->pending == t) o->pending = 0;
        o->started = 0;
        o->kind = OB_NONE;
    }
    if (t) release_failed_start_locked(&t->resources);
    pthread_mutex_unlock(&g_lock);
    free(t);
}

void mem_altstack_install(void);
/* Konuk thread'leri BUYUK yigin ister: lift edilmis kodda her konuk cagrisi
 * bir host C cagrisi, ve lifted cerceveler genis.  Ana konuk thread'e
 * (tests/boot.c) 1 GB veriliyordu ama oyunun CreateThread ile actigi isci
 * thread'leri `pthread_create(...,0,...)` ile macOS varsayilani 512 KB
 * aliyordu -> derin ozyinelemede HOST YIGIN TASMASI (SIGSEGV, guest alani
 * disinda bir adres).  Masaustu 256 MB rezervasyon kullanir; iOS'ta her oyun
 * thread'ine 16 MB baslangic butcesi verilir.  Bunlar host yiginlaridir,
 * guest'in Windows yigin boyutundan ayri tutulur. */
static int guest_pthread_create(pthread_t *th, void *(*fn)(void*), void *arg){
    pthread_attr_t at;
    int rc = pthread_attr_init(&at);
#if TARGET_OS_IPHONE
    const size_t stack_size = 16ull * 1024ull * 1024ull;
    if (rc != 0) return rc;
    rc = pthread_attr_setstacksize(&at, stack_size);
    if (rc != 0){ pthread_attr_destroy(&at); return rc; }
#else
    const size_t stack_size = 256ull * 1024ull * 1024ull;
    if (rc != 0) return pthread_create(th, 0, fn, arg);
    rc = pthread_attr_setstacksize(&at, stack_size);
    if (rc != 0){
        pthread_attr_destroy(&at);
        return pthread_create(th, 0, fn, arg);
    }
#endif
    rc = pthread_create(th, &at, fn, arg);
    pthread_attr_destroy(&at);
#if !TARGET_OS_IPHONE
    if (rc != 0) rc = pthread_create(th, 0, fn, arg);   /* son care */
#endif
    return rc;
}

static void *guest_thread_main(void *p){
    mem_altstack_install();
    TArg *t = (TArg*)p;
    const uint32_t teb = t->resources.teb;
    const uint32_t stack_lo = t->resources.stack_low;
    const uint32_t stack_hi = t->resources.stack_high;
    CPU cpu; memset(&cpu,0,sizeof cpu);
    jmp_buf exit_scope;
    void * volatile previous_exit_scope = win32_exit_scope_enter(&exit_scope);
    volatile int guest_lock_held = 0;
    int exit_kind = setjmp(exit_scope);
    if (exit_kind == 0){
        cpu.esp = stack_hi - 0x1000u;
        cpu.top = 0; cpu.fcw = 0x037f;
        cpu.fs_base = teb; cpu.gs_base = teb;
        /* TEB */
        MR32(teb+0x00)=0xFFFFFFFFu; MR32(teb+0x04)=stack_hi; MR32(teb+0x08)=stack_lo;
        MR32(teb+0x18)=teb; MR32(teb+0x24)=t->ob?t->ob->tid:0;
        MR32(teb+0x2C)=teb+0x1000u; MR32(teb+0x30)=0x00C10000u;
        /* stdcall: arg, sonra donus adresi */
        { CPU *c=&cpu; PUSH32(t->arg); PUSH32(0xDEAD7EADu); }
        __sync_fetch_and_add(&g_threads_started, 1);
        { static int sh=0; if (getenv("VERBOSE") && sh<40){ sh++;
            fprintf(stderr,"      [thread] BASLADI entry=0x%08x arg=0x%08x teb=0x%08x tid=%u\n",
                    t->entry, t->arg, teb, t->ob?t->ob->tid:0); } }
        tl_obj = t->ob;
        /* tani: thread listelerinde (perflog thread.csv, sample) taninsin */
        { char nm[32]; snprintf(nm, sizeof nm, "konuk %u @%06x", t->ob ? t->ob->tid : 0u, t->entry & 0xFFFFFFu);
          pthread_setname_np(nm); }
        guest_lock(&cpu);
        guest_lock_held = 1;
        dispatch_indirect(&cpu, t->entry);
        guest_unlock();
        guest_lock_held = 0;
    }
    if (guest_lock_held){ guest_unlock(); guest_lock_held = 0; }
    win32_exit_scope_leave((void *)previous_exit_scope);
    unreg_cpu(&cpu);
    if (t->ob){ pthread_mutex_lock(&t->ob->mtx); t->ob->signaled=1;
                pthread_cond_broadcast(&t->ob->cv); pthread_mutex_unlock(&t->ob->mtx); }
    pthread_mutex_lock(&g_lock);
    release_quiesced_locked(&t->resources);
    pthread_mutex_unlock(&g_lock);
    free(t);
    return 0;
}

void win32_signal_event(uint32_t h){
    Obj *o = obj_get(h);
    if (!o || o->kind != OB_EVENT) return;
    pthread_mutex_lock(&o->mtx);
    o->signaled = 1;
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->mtx);
}

/* ---- WINMM coklu ortam zamanlayicisi -----------------------------------
 * NFSU2 on yuzunun guncelleme dongusunu timeSetEvent geri cagirmasindan
 * suruyor; stub birakilirsa menu animasyonlari ilerlemez ve girdi
 * islenmez.  Oyun tek-seferlik zamanlayiciyi geri cagirma icinde yeniden
 * kuruyor, bu yuzden zamanlayici basina thread acmak olmaz: TEK bir
 * zamanlayici thread'i tum kayitlari sirayla isler.                     */
#define MAXTIMER 32
typedef struct {
    uint32_t proc, user, id;
    uint64_t due_us;          /* 0 = bos */
    uint32_t period_us;       /* 0 = tek seferlik */
} Timer;

static Timer    g_tm[MAXTIMER];
static uint32_t g_timer_next_id = 1;
static int      g_timer_running = 0;
uint64_t        g_timer_ticks = 0;

/* Zamanlayicilar da OYUN zamanini kullanmali; yoksa emulator yavas
 * oldugunda geri cagirmalar oyunun kendi zamanina gore cok sik calisir. */
uint64_t host_us(void);
static uint64_t now_us(void){ return host_us(); }

static void *timer_thread_main(void *p){
    TimerThreadStart *start = (TimerThreadStart*)p;
    Win32GuestThreadReservation resources = start->resources;
    const uint32_t teb = resources.teb;
    const uint32_t stack_lo = resources.stack_low;
    const uint32_t stack_hi = resources.stack_high;
    free(start);
    mem_altstack_install();
    CPU cpu; memset(&cpu,0,sizeof cpu);
    jmp_buf exit_scope;
    void * volatile previous_exit_scope = win32_exit_scope_enter(&exit_scope);
    volatile int guest_lock_held = 0;
    int exit_kind = setjmp(exit_scope);
    if (exit_kind == 0){
        cpu.top = 0; cpu.fcw = 0x037f;
        cpu.fs_base = teb; cpu.gs_base = teb;
        MR32(teb+0x00)=0xFFFFFFFFu; MR32(teb+0x04)=stack_hi; MR32(teb+0x08)=stack_lo;
        MR32(teb+0x18)=teb; MR32(teb+0x24)=999u;
        MR32(teb+0x2C)=teb+0x1000u; MR32(teb+0x30)=0x00C10000u;

        for (;;){
            struct timespec ts = {0, 1000000L};      /* 1 ms */
            nanosleep(&ts, 0);
            /* Timers may be absent, one-shot, or idle. Observe process exit
             * outside both the guest lock and timer-table mutex so cleanup
             * can unwind this worker's own scope safely. */
            win32_exit_checkpoint(&cpu);
            uint64_t now = now_us();
            for (int i = 0; i < MAXTIMER; i++){
                uint32_t proc, user, id;
                pthread_mutex_lock(&g_lock);
                if (!g_tm[i].due_us || g_tm[i].due_us > now){ pthread_mutex_unlock(&g_lock); continue; }
                proc = g_tm[i].proc; user = g_tm[i].user; id = g_tm[i].id;
                if (g_tm[i].period_us) g_tm[i].due_us = now + g_tm[i].period_us;
                else                   g_tm[i].due_us = 0;      /* tek seferlik biter */
                pthread_mutex_unlock(&g_lock);

                cpu.esp = stack_hi - 0x1000u;
                { CPU *c=&cpu;
                  /* stdcall: dw2, dw1, dwUser, uMsg, uTimerID  (sagdan sola) */
                  PUSH32(0u); PUSH32(0u); PUSH32(user); PUSH32(0u); PUSH32(id);
                  PUSH32(0xDEAD7EADu); }
                guest_lock(&cpu);
                guest_lock_held = 1;
                dispatch_indirect(&cpu, proc);
                guest_unlock();
                guest_lock_held = 0;
                g_timer_ticks++;
            }
        }
    }
    if (guest_lock_held){ guest_unlock(); guest_lock_held = 0; }
    win32_exit_scope_leave((void *)previous_exit_scope);
    unreg_cpu(&cpu);
    pthread_mutex_lock(&g_lock);
    g_timer_running = 0;
    release_quiesced_locked(&resources);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

/* ---- registry: bellek ici, "bulunamadi" varsayilani ---- */
/* Oyun degeri bulamazsa kendi varsayilanini kullanir - istedigimiz bu. */
#define ERROR_SUCCESS         0u
#define ERROR_FILE_NOT_FOUND  2u

/* ---- disari acilan API ---- */
static uint32_t A(CPU *c, int i){ return MR32(c->esp + 4u*(uint32_t)i); }

/* --- basit kalici kayit defteri ---------------------------------------
 * Oyunun yazdigi ayarlar (grafik detayi, cozunurluk, ses) eskiden
 * atiliyordu.  Burada tutulup ~/.nfsu2_registry dosyasina yaziliyor. */
#define MAXREG 128
static struct { char name[64]; int is_str; uint32_t dw; char sv[128]; } g_reg[MAXREG];
static int g_regn = 0;
static int g_reg_loaded = 0;

static const char *reg_path(void){
    static char pth[512];
    const char *h = getenv("HOME");
    snprintf(pth, sizeof pth, "%s/.nfsu2_registry", h ? h : ".");
    return pth;
}
static void reg_load(void){
    if (g_reg_loaded) return;
    g_reg_loaded = 1;
    FILE *f = fopen(reg_path(), "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f) && g_regn < MAXREG){
        char nm[64], val[128]; int isstr = 0; unsigned dw = 0;
        if (sscanf(line, "d %63[^=]=%u", nm, &dw) == 2){
            snprintf(g_reg[g_regn].name, 64, "%s", nm);
            g_reg[g_regn].is_str = 0; g_reg[g_regn].dw = dw; g_regn++;
        } else if (sscanf(line, "s %63[^=]=%127[^\n]", nm, val) == 2){
            snprintf(g_reg[g_regn].name, 64, "%s", nm);
            snprintf(g_reg[g_regn].sv, 128, "%s", val);
            g_reg[g_regn].is_str = 1; g_regn++;
        }
        (void)isstr;
    }
    fclose(f);
}
static void reg_save(void){
    FILE *f = fopen(reg_path(), "w");
    if (!f) return;
    for (int i = 0; i < g_regn; i++){
        if (g_reg[i].is_str) fprintf(f, "s %s=%s\n", g_reg[i].name, g_reg[i].sv);
        else                 fprintf(f, "d %s=%u\n", g_reg[i].name, g_reg[i].dw);
    }
    fclose(f);
}
static int reg_find(const char *nm){
    reg_load();
    for (int i = 0; i < g_regn; i++)
        if (!strcasecmp(g_reg[i].name, nm)) return i;
    return -1;
}
static void reg_put_dword(const char *nm, uint32_t v){
    int i = reg_find(nm);
    if (i < 0){ if (g_regn >= MAXREG) return; i = g_regn++;
                snprintf(g_reg[i].name, 64, "%s", nm); }
    g_reg[i].is_str = 0; g_reg[i].dw = v; reg_save();
}
static void reg_put_string(const char *nm, const char *v){
    int i = reg_find(nm);
    if (i < 0){ if (g_regn >= MAXREG) return; i = g_regn++;
                snprintf(g_reg[i].name, 64, "%s", nm); }
    g_reg[i].is_str = 1; snprintf(g_reg[i].sv, 128, "%s", v); reg_save();
}

#if TARGET_OS_IPHONE
/* Fresh iOS profiles start with the checked NFSRef full-detail values. These are
 * the game's own registry settings: racing resolution row 3 is ROW_NATIVE in
 * com.c; the reference profile stores world LOD 3 and car LOD 1. Because
 * NotFirstTime=1 bypasses sub_005be880's preset init and goes directly to
 * sub_005bea30's 27-key registry loader, all 27 keys must be provided so
 * unpopulated settings do not remain at their .bss zero (OFF) state. Missing
 * values are saved once so later launches keep user changes. */
static int reg_default_graphics_dword(const char *name, uint32_t *value){
    static const struct { const char *key; uint32_t val; } k_defaults[] = {
        { "g_RacingResolution",            3u },
        { "g_WorldLodLevel",               3u },
        { "g_CarLodLevel",                 1u },
        { "g_PerformanceLevel",            5u },
        { "g_CarEnvironmentMapEnable",     3u },
        { "g_CarEnvironmentMapUpdateData", 1u },
        { "g_CarShadowEnable",             2u },
        { "g_CarHeadlightEnable",          1u },
        { "g_CarLightingEnable",           0u },
        { "g_CarDamageEnable",             0u },
        { "g_CrowdEnable",                 1u },
        { "g_RoadReflectionEnable",        3u },
        { "g_FogEnable",                   1u },
        { "g_MotionBlurEnable",            1u },
        { "g_LightStreaksEnable",          1u },
        { "g_LightGlowEnable",             1u },
        { "g_AnimatedTextureEnable",       1u },
        { "g_ParticleSystemEnable",        1u },
        { "g_DepthOfFieldEnable",          1u },
        { "g_OverBrightEnable",            1u },
        { "g_BleachByPassEnable",          1u },
        { "g_TintingEnable",               1u },
        { "g_FSAALevel",                   0u },
        { "g_HorizonFogEnable",            1u },
        { "g_RainEnable",                  1u },
        { "g_TextureFiltering",            2u },
        { "g_VSyncOn",                     0u },
    };
    for (unsigned i = 0; i < sizeof(k_defaults) / sizeof(k_defaults[0]); i++){
        if (!strcasecmp(name, k_defaults[i].key)){
            *value = k_defaults[i].val;
            return 1;
        }
    }
    return 0;
}
#endif

int win32_ext_call(CPU *c, const char *n, uint32_t *out){
    uint32_t r = 0;
    static int s_reglog = -1;
    if (s_reglog < 0) s_reglog = getenv("REGLOG") != 0;
    /* --- Registry --- */
    if (!strcmp(n,"RegOpenKeyA")){ uint32_t p=A(c,2); if(p) MR32(p)=obj_new(OB_KEY); r=ERROR_SUCCESS; }
    else if (!strcmp(n,"RegOpenKeyExA")){
        if (s_reglog){ uint32_t sp=A(c,1);
            fprintf(stderr,"  [kayit] OpenKeyEx(\"%s\")\n",
                    (sp && mem_is_committed(sp)) ? (const char*)(g_mem+sp) : "(null)"); }
        uint32_t p=A(c,4); if(p) MR32(p)=obj_new(OB_KEY); r=ERROR_SUCCESS; }
    else if (!strcmp(n,"RegCreateKeyA")){ uint32_t p=A(c,2); if(p) MR32(p)=obj_new(OB_KEY); r=ERROR_SUCCESS; }
    else if (!strcmp(n,"RegCreateKeyExA")){ uint32_t p=A(c,7); if(p) MR32(p)=obj_new(OB_KEY); r=ERROR_SUCCESS; }
    else if (!strcmp(n,"RegQueryValueExA")){
        /* Bos bir kayit defteri oyunu HER ACILISTA "ilk kurulum" yoluna
         * sokuyor (NotFirstTime bulunamiyor) ve kurulum bilgisi olmadan
         * on yuz akisi ilerlemiyor.  Makul bir kurulmus-oyun profili
         * donduruyoruz. */
        uint32_t nameptr=A(c,1), ptype=A(c,3), pdata=A(c,4), pcb=A(c,5);
        const char *vn = (nameptr && mem_is_committed(nameptr))
                         ? (const char*)(g_mem+nameptr) : "";
        if (s_reglog)
            fprintf(stderr,"  [kayit] QueryValue(\"%s\")\n", vn);

        const char *sval = 0; uint32_t dval = 0; int have_d = 0;
        { int ri = reg_find(vn);            /* once KAYITLI degere bak */
          if (ri >= 0){
              if (g_reg[ri].is_str) sval = g_reg[ri].sv;
              else { dval = g_reg[ri].dw; have_d = 1; }
          } }
        if (sval || have_d) { /* kayitli deger kullanilacak */ }
        else if (!strcasecmp(vn,"NotFirstTime")){ dval=1; have_d=1; }
        else if (!strcasecmp(vn,"Language"))      sval="English";
        else if (!strcasecmp(vn,"Install Dir"))   sval="C:\\NFSU2";
        else if (!strcasecmp(vn,"CD Drive"))      sval="C:\\NFSU2";
        else if (!strcasecmp(vn,"StreamingInstall")){ dval=0; have_d=1; }
        else if (!strcasecmp(vn,"VERSION"))       sval="1.2.0.0";
        else if (!strcasecmp(vn,"SIZE")){ dval=0; have_d=1; }
#if TARGET_OS_IPHONE
        else if (reg_default_graphics_dword(vn, &dval)){
            reg_put_dword(vn, dval); have_d=1;
        }
#endif

        if (sval){
            uint32_t need = (uint32_t)strlen(sval) + 1u;
            if (ptype){ mem_commit(ptype,4); MR32(ptype)=1u; }      /* REG_SZ */
            if (!pdata){ if(pcb){ mem_commit(pcb,4); MR32(pcb)=need; } r=ERROR_SUCCESS; }
            else {
                uint32_t cap = pcb ? MR32(pcb) : need;
                if (cap < need){ if(pcb) MR32(pcb)=need; r=234u; } /* ERROR_MORE_DATA */
                else { mem_commit(pdata,need); memcpy(g_mem+pdata, sval, need);
                       if(pcb) MR32(pcb)=need; r=ERROR_SUCCESS; }
            }
        } else if (have_d){
            if (ptype){ mem_commit(ptype,4); MR32(ptype)=4u; }      /* REG_DWORD */
            if (!pdata){ if(pcb){ mem_commit(pcb,4); MR32(pcb)=4u; } r=ERROR_SUCCESS; }
            else {
                uint32_t cap = pcb ? MR32(pcb) : 4u;
                if (cap < 4u){ if(pcb) MR32(pcb)=4u; r=234u; }
                else { mem_commit(pdata,4); MR32(pdata)=dval;
                       if(pcb) MR32(pcb)=4u; r=ERROR_SUCCESS; }
            }
        } else {
            if(pcb){ mem_commit(pcb,4); MR32(pcb)=0; }
            r=ERROR_FILE_NOT_FOUND;
        }
    }
    else if (!strcmp(n,"RegQueryValueA")){ r=ERROR_FILE_NOT_FOUND; }
    else if (!strcmp(n,"RegSetValueExA")||!strcmp(n,"RegSetValueA")){
        /* Eskiden yazilanlar SESSIZCE ATILIYORDU: oyunun kaydettigi
         * grafik/ses ayarlari her acilista sifirlaniyordu.  Artik
         * saklaniyor ve diske yaziliyor. */
        uint32_t nameptr=A(c,1), type=A(c,3), pdata=A(c,4), cb=A(c,5);
        const char *vn = (nameptr && mem_is_committed(nameptr))
                         ? (const char*)(g_mem+nameptr) : "";
        if (*vn){
            if (type == 4u && pdata && mem_is_committed(pdata+3))
                reg_put_dword(vn, MR32(pdata));
            else if (type == 1u && pdata && mem_is_committed(pdata))
                reg_put_string(vn, (const char*)(g_mem+pdata));
            if (getenv("REGLOG"))
                fprintf(stderr,"  [kayit] SetValue(\"%s\") tur=%u boyut=%u\n", vn, type, cb);
        }
        r=ERROR_SUCCESS; }
    else if (!strcmp(n,"RegCloseKey")) r=ERROR_SUCCESS;
    else if (!strcmp(n,"RegDeleteKeyA")||!strcmp(n,"RegDeleteValueA")) r=ERROR_SUCCESS;
    else if (!strcmp(n,"RegEnumKeyExA")||!strcmp(n,"RegEnumValueA")) r=259u; /* ERROR_NO_MORE_ITEMS */
    /* --- pencere / USER32 --- */
    /* Pencere mesaj yolu.  DirectInput cihazlari yalnizca uygulama ODAK
     * aldiginda Acquire ediliyor; oyun bunu WM_ACTIVATEAPP/WM_SETFOCUS
     * mesajlarindan anliyor.  PeekMessage hep 0 dondugu ve DispatchMessage
     * pencere yordamini hic cagirmadigi icin oyun hicbir zaman "aktifim"
     * demiyordu -> klavye hic Acquire edilmiyordu.                      */

    else if (!strcmp(n,"RegisterClassExA")||!strcmp(n,"RegisterClassA")){
        /* WNDCLASSEXA: +8 lpfnWndProc | WNDCLASSA: +4 lpfnWndProc */
        uint32_t wc=A(c,0);
        if (wc && mem_is_committed(wc+11)){
            uint32_t off = !strcmp(n,"RegisterClassExA") ? 8u : 4u;
            uint32_t wp = MR32(wc+off);
            if (wp) { g_wndproc = wp;
                fprintf(stderr,"  [pencere] sinif kaydi, yordam=0x%08x\n", wp); }
        }
        r=0xC001u; }
    else if (!strcmp(n,"UnregisterClassA")) r=1;
    else if (!strcmp(n,"CreateWindowExA")){
        r = g_hwnd;
        if (!g_wm_activated){ g_wm_activated = 1;
            wm_push(0x0018u, 1u, 0u);   /* WM_SHOWWINDOW  */
            wm_push(0x001Cu, 1u, 0u);   /* WM_ACTIVATEAPP  aktif */
            wm_push(0x0006u, 1u, 0u);   /* WM_ACTIVATE     WA_ACTIVE */
            wm_push(0x0007u, 0u, 0u);   /* WM_SETFOCUS     */
            wm_push(0x000Fu, 0u, 0u);   /* WM_PAINT        */
        } }
    else if (!strcmp(n,"DestroyWindow")) r=1;
    else if (!strcmp(n,"GetDesktopWindow")) r=0x00B00000u;
    else if (!strcmp(n,"GetActiveWindow")||!strcmp(n,"GetForegroundWindow")) r=0x00B00001u;
    else if (!strcmp(n,"LoadIconA")||!strcmp(n,"LoadCursorA")) r=0x00B10001u;
    else if (!strcmp(n,"SetCursor")) r=0x00B10001u;
    else if (!strcmp(n,"ShowCursor")){ static int cnt=0; r=(uint32_t)(A(c,0)?++cnt:--cnt); }
    else if (!strcmp(n,"GetCursorPos")){ uint32_t p=A(c,0);
        if(p){ int mx=0,my=0; void metal_get_cursor(int*,int*); metal_get_cursor(&mx,&my);
               mem_commit(p,8); MR32(p)=(uint32_t)mx; MR32(p+4)=(uint32_t)my; } r=1; }
    else if (!strcmp(n,"SetCursorPos")) r=1;
    else if (!strcmp(n,"GetSystemMetrics")){
        unsigned gw = 1280, gh = 720; void metal_game_size(unsigned*,unsigned*); metal_game_size(&gw,&gh);
        switch(A(c,0)){ case 0: r=gw; break; case 1: r=gh; break;
                        case 4: r=19; break; case 5: r=1; break; case 6: r=1; break;
                        case 32: r=4; case 33: r=4; break; default: r=0; } }
    else if (!strcmp(n,"AdjustWindowRect")){ r=1; }
    else if (!strcmp(n,"GetClientRect")||!strcmp(n,"GetWindowRect")){
        /* Oyunun gercek cozunurlugu: imlec eslemesi buna gore (bkz.
         * metal_game_size). */
        unsigned gw = 1280, gh = 720; void metal_game_size(unsigned*,unsigned*); metal_game_size(&gw,&gh);
        uint32_t p=A(c,1); if(p){mem_commit(p,16);MR32(p)=0;MR32(p+4)=0;MR32(p+8)=gw;MR32(p+12)=gh;} r=1; }
    else if (!strcmp(n,"ShowWindow")||!strcmp(n,"UpdateWindow")||!strcmp(n,"SetFocus")
           ||!strcmp(n,"SetForegroundWindow")||!strcmp(n,"SetWindowPos")
           ||!strcmp(n,"InvalidateRect")||!strcmp(n,"SetWindowTextA")) r=1;
    else if (!strcmp(n,"SetWindowLongA")||!strcmp(n,"GetWindowLongA")) r=0;
    else if (!strcmp(n,"PeekMessageA")||!strcmp(n,"GetMessageA")){
        /* MSG: +0 hwnd +4 message +8 wParam +12 lParam +16 time +20 pt */
        uint32_t pm=A(c,0), m=0, wp=0, lp=0;
        int remove = !strcmp(n,"GetMessageA") ? 1 : (A(c,4) & 1u);
        wm_poll_input();
        if (g_wmq_head != g_wmq_tail){
            if (remove) wm_pop(&m,&wp,&lp);
            else { m=g_wmq[g_wmq_head].msg; wp=g_wmq[g_wmq_head].wp; lp=g_wmq[g_wmq_head].lp; }
            if (pm){ mem_commit(pm,28);
                int mx=0, my=0; metal_get_cursor(&mx, &my);
                MR32(pm)=g_hwnd; MR32(pm+4)=m; MR32(pm+8)=wp; MR32(pm+12)=lp;
                MR32(pm+16)=0; MR32(pm+20)=(uint32_t)mx; MR32(pm+24)=(uint32_t)my; }
            g_wm_sent++;
            r=1;
        } else {
            if (pm){ mem_commit(pm,28); memset(g_mem+pm,0,28); MR32(pm)=g_hwnd; }
            r = !strcmp(n,"GetMessageA") ? 1u : 0u;   /* GetMessage 0 = WM_QUIT */
        }
    }
    else if (!strcmp(n,"DispatchMessageA")){
        uint32_t pm=A(c,0);
        if (g_wndproc && pm && mem_is_committed(pm+15)){
            uint32_t args[4] = { MR32(pm), MR32(pm+4), MR32(pm+8), MR32(pm+12) };
            if (args[1]){ g_wm_dispatched++; r = call_guest(c, g_wndproc, 4, args); }
        }
    }
    else if (!strcmp(n,"TranslateMessage")
           ||!strcmp(n,"DefWindowProcA")||!strcmp(n,"PostMessageA")
           ||!strcmp(n,"SendMessageA")||!strcmp(n,"PostQuitMessage")) r=0;
    else if (!strcmp(n,"ReleaseCapture")||!strcmp(n,"SetCapture")) r=1;
    else if (!strcmp(n,"IsIconic")) r=0;
    else if (!strcmp(n,"GetKeyboardState")){
        uint32_t p=A(c,0);
        if(p){
            uint8_t ks[256]={0}, ms[20]={0};
            metal_fill_keyboard(ks, sizeof(ks));
            metal_fill_mouse(ms, sizeof(ms));
            mem_commit(p,256);
            memset(g_mem+p,0,256);
            static const uint8_t k_vks[] = {
                0x01u, 0x02u, 0x08u, 0x09u, 0x0Du, 0x10u, 0x11u, 0x12u,
                0x1Bu, 0x20u, 0x25u, 0x26u, 0x27u, 0x28u, 0xA0u, 0xA1u, 0xA2u, 0xA3u
            };
            for (unsigned i = 0; i < sizeof(k_vks); i++){
                uint8_t vk = k_vks[i];
                if (win32_vk_is_down(vk, ks, ms)) g_mem[p + vk] = 0x80u;
            }
        }
        r=1;
    }
    else if (!strcmp(n,"GetAsyncKeyState")||!strcmp(n,"GetKeyState")){
        uint32_t vk = A(c,0) & 0xFFu;
        uint8_t ks[256]={0}, ms[20]={0};
        metal_fill_keyboard(ks, sizeof(ks));
        metal_fill_mouse(ms, sizeof(ms));
        int down = win32_vk_is_down(vk, ks, ms);
        if (!strcmp(n,"GetAsyncKeyState")){
            static uint8_t async_prev[256] = {0};
            int edge = down && !async_prev[vk];
            async_prev[vk] = (uint8_t)down;
            r = down ? (0x8000u | (edge ? 1u : 0u)) : 0u;
        } else {
            r = down ? 0xFF80u : 0u;
        }
    }
    /* --- senkronizasyon --- */
    else if (!strcmp(n,"CreateEventA")){
        uint32_t h=obj_new(OB_EVENT); Obj*o=obj_get(h);
        if(o){ o->manual=(int)A(c,1); o->signaled=(int)A(c,2); } r=h; }
    else if (!strcmp(n,"SetEvent")){ Obj*o=obj_get(A(c,0));
        { uint32_t h=A(c,0);
          if (h>=H_BASE && h<H_BASE+64){ g_wstat[h-H_BASE].sets++;
            g_wstat[h-H_BASE].setter_tid = cur_tid(c); } }
        if(o){ pthread_mutex_lock(&o->mtx); o->signaled=1; pthread_cond_broadcast(&o->cv); pthread_mutex_unlock(&o->mtx);} r=1; }
    else if (!strcmp(n,"ResetEvent")){ Obj*o=obj_get(A(c,0));
        if(o){ pthread_mutex_lock(&o->mtx); o->signaled=0; pthread_mutex_unlock(&o->mtx);} r=1; }
    else if (!strcmp(n,"CreateMutexA")){
        uint32_t h=obj_new(OB_MUTEX); Obj*o=obj_get(h);
        if(o){ if (A(c,1)){ o->signaled=0; o->owner_tid=cur_tid(c); o->rec=1; }
               else       { o->signaled=1; o->owner_tid=0; o->rec=0; } }
        r=h; }
    else if (!strcmp(n,"ReleaseMutex")){
        Obj*o=obj_get(A(c,0)); uint32_t me=cur_tid(c);
        r=0;
        if(o){ pthread_mutex_lock(&o->mtx);
               if (o->kind == OB_MUTEX){
                   if (o->owner_tid == me && o->rec > 0){
                       if (--o->rec == 0){ o->owner_tid = 0; o->signaled = 1;
                                           pthread_cond_broadcast(&o->cv); }
                       r=1;
                   } else r=0;            /* sahibi degiliz */
               } else { o->signaled=1; pthread_cond_broadcast(&o->cv); r=1; }
               pthread_mutex_unlock(&o->mtx); }
    }
    else if (!strcmp(n,"CreateSemaphoreA")){ uint32_t h=obj_new(OB_SEM); Obj*o=obj_get(h); if(o) o->signaled=(int)A(c,1); r=h; }
    else if (!strcmp(n,"ReleaseSemaphore")){ Obj*o=obj_get(A(c,0));
        if(o){ pthread_mutex_lock(&o->mtx); o->signaled+=(int)A(c,1); pthread_cond_broadcast(&o->cv); pthread_mutex_unlock(&o->mtx);} r=1; }
    else if (!strcmp(n,"WaitForSingleObject")){
        Obj*o=obj_get(A(c,0)); uint32_t ms=A(c,1);
        { uint32_t h=A(c,0);
          uint32_t tid = cur_tid(c);
          if (h>=H_BASE && h<H_BASE+64){ g_wstat[h-H_BASE].waits++;
                                         g_wstat[h-H_BASE].waiter_tid = tid; }
          static unsigned sh=0;
          if (++sh % 400 == 0){
#if NFS_FULL_RUNTIME
            stall_checkpoint();
#endif
            /* Full handle tables produced thousands of synchronous writes
             * during normal play. Keep them available for an explicit capture;
             * routine performance summaries are emitted by the session logger. */
            static int waitlog = -1;
            if (waitlog < 0){
                const char *value = getenv("WAITLOG");
                waitlog = value && *value && *value != '0';
            }
            if (waitlog){
            fprintf(stderr,"  [olay] wall_ms=%llu handle tur sinyal bekleme SetEvent bekleyen-tid setter-tid owner rec state\n",
                    (unsigned long long)diagnostic_wall_ms());
            for (int i=0;i<64;i++) if (g_wstat[i].waits || g_wstat[i].sets){
                int kind = g_obj[i].kind;
                int signaled = -1, manual = -1, locked = 0;
                uint32_t owner = 0; int rec = 0;
                if (kind && pthread_mutex_trylock(&g_obj[i].mtx) == 0){
                    locked = 1;
                    signaled = g_obj[i].signaled;
                    manual = g_obj[i].manual;
                    owner = g_obj[i].owner_tid;
                    rec = g_obj[i].rec;
                    pthread_mutex_unlock(&g_obj[i].mtx);
                }
                const char *state = !kind ? "missing" : locked ? "ok" : "busy";
                fprintf(stderr, "         h%-4d %3d %6d %8llu %9llu  tid=%u set=%u owner=%u rec=%d manual=%d state=%s\n", i,
                        kind, signaled,
                        (unsigned long long)g_wstat[i].waits,
                        (unsigned long long)g_wstat[i].sets,
                        g_wstat[i].waiter_tid, g_wstat[i].setter_tid,
                        owner, rec, manual, state);
            }
            }
          } }
        if(!o){ r=0; }
        else if (o->kind == OB_MUTEX){
            /* MUTEX: sahiplik + OZYINELEME.  Ayni thread ayni mutex'i tekrar
             * alabilmeli; yoksa oyunun ic ice kilit alan CRT sarmalayicisi
             * kendi kendini kilitliyor (video oynatici boyle donuyordu). */
            uint32_t me = cur_tid(c);
            const int finite = ms != WIN32_WAIT_INFINITE;
            struct timespec deadline;
            int deadline_ok = !finite || !ms ||
                win32_wait_deadline_after_ms(&deadline, ms);
            guest_yield_begin();
            pthread_mutex_lock(&o->mtx);
            if (o->owner_tid == me && o->rec > 0){
                o->rec++; r = 0;                       /* WAIT_OBJECT_0 */
            } else {
                int timed_out = 0;
                int wait_failed = 0;
                int process_exit_pending = win32_exit_requested();
                while (!process_exit_pending && !o->signaled && !timed_out){
                    if (win32_exit_requested()){
                        process_exit_pending = 1;
                        break;
                    }
                    if (!ms){ timed_out = 1; break; }
                    if (finite){
                        if (!deadline_ok){ wait_failed = 1; break; }
                        int expired = win32_wait_deadline_expired(&deadline);
                        if (expired < 0){ wait_failed = 1; break; }
                        if (expired){ timed_out = 1; break; }
                    }
                    if (win32_wait_cond_step(o, &deadline, finite) != 0){
                        wait_failed = 1;
                        break;
                    }
                    if (win32_exit_requested()){
                        process_exit_pending = 1;
                        break;
                    }
                }
                if (!process_exit_pending && o->signaled){
                    o->signaled = 0; o->owner_tid = me; o->rec = 1; r = 0;
                }
                else r = wait_failed ? WIN32_WAIT_FAILED : 0x102u;
                if (!process_exit_pending && win32_exit_requested())
                    process_exit_pending = 1;
                if (process_exit_pending) r = 0x102u;
            }
            /* A scope jump is safe only after releasing this object mutex and
             * restoring the guest lock expected by the worker's cleanup. */
            pthread_mutex_unlock(&o->mtx);
            guest_yield_end();
            win32_exit_checkpoint(c);
            if (r == WIN32_WAIT_FAILED)
                win32_set_last_error(c, WIN32_ERROR_GEN_FAILURE);
        }
        else {
            const int finite = ms != WIN32_WAIT_INFINITE;
            struct timespec deadline;
            int deadline_ok = !finite || !ms ||
                win32_wait_deadline_after_ms(&deadline, ms);
            int process_exit_pending = 0;
            int wait_failed = 0;
            guest_yield_begin();
            pthread_mutex_lock(&o->mtx);
            while (!win32_wait_object_signaled(o)){
                if (!ms) break;
                if (win32_exit_requested()){
                    process_exit_pending = 1;
                    break;
                }
                if (finite){
                    if (!deadline_ok){ wait_failed = 1; break; }
                    int expired = win32_wait_deadline_expired(&deadline);
                    if (expired < 0){ wait_failed = 1; break; }
                    if (expired) break;
                }
                if (win32_wait_cond_step(o, &deadline, finite) != 0){
                    wait_failed = 1;
                    break;
                }
                if (win32_exit_requested()){
                    process_exit_pending = 1;
                    break;
                }
            }
            if (!process_exit_pending && win32_wait_object_signaled(o)){
                r = 0u;                                 /* WAIT_OBJECT_0 */
                win32_wait_object_consume(o);
            } else r = wait_failed ? WIN32_WAIT_FAILED : 0x102u;
            if (!process_exit_pending && win32_exit_requested())
                process_exit_pending = 1;
            if (process_exit_pending) r = 0x102u;
            pthread_mutex_unlock(&o->mtx);
            guest_yield_end();
            if (process_exit_pending) win32_exit_checkpoint(c);
            if (r == WIN32_WAIT_FAILED)
                win32_set_last_error(c, WIN32_ERROR_GEN_FAILURE);
        } }
    /* --- thread --- */
    else if (!strcmp(n,"CreateThread")){
        uint32_t entry=A(c,2), arg=A(c,3), ptid=A(c,5);
        { static int sh=0; if (getenv("VERBOSE") && sh<40){ sh++;
            fprintf(stderr,"      [thread] CreateThread(attr=0x%08x stack=0x%x entry=0x%08x arg=0x%08x flags=0x%x ptid=0x%08x)\n",
                    A(c,0),A(c,1),entry,arg,A(c,4),ptid); } }
        uint32_t h=obj_new(OB_THREAD); Obj*o=obj_get(h);
        static uint32_t next_tid=100;
        r=0;
        if(o){
            TArg *t = calloc(1,sizeof *t);
            int reserve_rc = ENOMEM;
            pthread_mutex_lock(&g_lock);
            if (t) reserve_rc = win32_guest_thread_resources_reserve(
                &g_guest_thread_resources, &t->resources);
            pthread_mutex_unlock(&g_lock);
            if (reserve_rc == 0){
                t->entry=entry; t->arg=arg; t->ob=o;
                prepare_guest_thread_resources(&t->resources);
                o->tid = ++next_tid;
                if (A(c,4) & 0x4u){             /* CREATE_SUSPENDED */
                    pthread_mutex_lock(&g_lock);
                    o->started = 0;
                    o->pending = t;
                    pthread_mutex_unlock(&g_lock);
                    if(ptid) MR32(ptid)=o->tid;
                    r=h;
                } else {
                    /* Claim the TArg before releasing g_lock. CloseHandle can
                     * then never free it while pthread_create is in flight. */
                    pthread_mutex_lock(&g_lock);
                    o->started = 1;
                    o->pending = 0;
                    pthread_mutex_unlock(&g_lock);
                    if (guest_pthread_create(&o->th,guest_thread_main,t)==0){
                        pthread_detach(o->th);
                        if(ptid) MR32(ptid)=o->tid;
                        r=h;
                    } else discard_failed_thread_start(o,t);
                }
            } else {
                free(t);
                pthread_mutex_lock(&g_lock);
                o->kind = OB_NONE;
                pthread_mutex_unlock(&g_lock);
            }
        }
    }
    else if (!strcmp(n,"GetCurrentThread")) r=0xFFFFFFFEu;
    else if (!strcmp(n,"ResumeThread")){
        Obj*o=obj_get(A(c,0));
        TArg *t=0;
        if (o){
            pthread_mutex_lock(&g_lock);
            if (o->kind==OB_THREAD)
                t=(TArg*)win32_guest_thread_claim_resume(
                    &o->pending, &o->started);
            pthread_mutex_unlock(&g_lock);
        }
        r=1;
        if (t){
            if (guest_pthread_create(&o->th,guest_thread_main,t)==0)
                pthread_detach(o->th);
            else { discard_failed_thread_start(o,t); r=0xFFFFFFFFu; }
        } }
    else if (!strcmp(n,"SuspendThread")) r=0;
    else if (!strcmp(n,"ExitThread")){
        /* Guest thread'i GERCEKTEN sonlandir. Aksi halde yurutme _endthreadex'ten
         * _threadstartex prologuna duser ve thread kendini yeniden baslatir.   */
        Obj *o = (Obj*)tl_obj;
        if (o){ pthread_mutex_lock(&o->mtx); o->signaled=1;
                pthread_cond_broadcast(&o->cv); pthread_mutex_unlock(&o->mtx); }
        __sync_fetch_and_add(&g_threads_exited, 1);
        if (!win32_exit_thread((int)A(c,0))){
            guest_unlock();
            pthread_exit(0);
        }
        r=1; }
    else if (!strcmp(n,"TerminateThread")) r=1;
    else if (!strcmp(n,"GetExitCodeThread")){ uint32_t p=A(c,1); if(p) MR32(p)=0; r=1; }
    else if (!strcmp(n,"SetThreadAffinityMask")||!strcmp(n,"SetProcessAffinityMask")) r=1;
    else if (!strcmp(n,"GetProcessAffinityMask")){
        uint32_t a=A(c,1),b=A(c,2); if(a) MR32(a)=0xFF; if(b) MR32(b)=0xFF; r=1; }
    else if (!strcmp(n,"SetPriorityClass")) r=1;
    else if (!strcmp(n,"GetPriorityClass")) r=0x20;
    /* --- handle / cesitli --- */
    else if (!strcmp(n,"Sleep")){
        uint32_t ms=A(c,0);
        guest_yield_begin();
        { struct timespec ts={ (time_t)(ms/1000u), (long)((ms%1000u)*1000000u) };
          if(!ms){ ts.tv_nsec = 200000L; }        /* Sleep(0): sadece yer ver */
          nanosleep(&ts,0); }
        guest_yield_end();
        r=0; }
    else if (!strcmp(n,"SleepEx")){
        uint32_t ms=A(c,0);
        guest_yield_begin();
        { struct timespec ts={ (time_t)(ms/1000u), (long)((ms%1000u)*1000000u) };
          if(!ms) ts.tv_nsec = 200000L;
          nanosleep(&ts,0); }
        guest_yield_end();
        r=0; }
    else if (!strcmp(n,"WaitForMultipleObjects")){
        uint32_t cnt=A(c,0), parr=A(c,1), waitall=A(c,2), ms=A(c,3);
        (void)waitall;
        guest_yield_begin();
        uint32_t got=0x102u;
        for (int spin=0; spin<200; spin++){
            for (uint32_t i=0;i<cnt && i<64;i++){
                Obj*o=obj_get(MR32(parr+4u*i));
                if(o){ pthread_mutex_lock(&o->mtx);
                       int sig=o->signaled;
                       if(sig && !o->manual) o->signaled=0;
                       pthread_mutex_unlock(&o->mtx);
                       if(sig){ got=i; break; } }
            }
            if (got!=0x102u || ms==0) break;
            struct timespec ts={0,1000000L}; nanosleep(&ts,0);
        }
        guest_yield_end();
        r=got; }
    else if (!strcmp(n,"SwitchToThread")){ guest_yield_begin(); sched_yield(); guest_yield_end(); r=1; }
    else if (!strcmp(n,"CloseHandle")){
        Obj *o=obj_get(A(c,0));
        TArg *pending=0;
        if (o){
            pthread_mutex_lock(&g_lock);
            if (o->kind==OB_THREAD){
                pending=(TArg*)win32_guest_thread_claim_close(
                    &o->pending, &o->started);
            }
            if (pending){
                o->kind=OB_NONE;
                release_failed_start_locked(&pending->resources);
            }
            pthread_mutex_unlock(&g_lock);
        }
        free(pending);
        r=1; }
    else if (!strcmp(n,"DuplicateHandle")){ uint32_t p=A(c,3); if(p) MR32(p)=A(c,1); r=1; }
    else if (!strcmp(n,"SetErrorMode")) r=0;
    else if (!strcmp(n,"GetLogicalDrives")) r=0x0000000Cu;             /* C: ve D: */
    else if (!strcmp(n,"GetDriveTypeA")) r=3;                          /* DRIVE_FIXED */
    else if (!strcmp(n,"GetCurrentDirectoryA")){
        uint32_t p=A(c,1); const char*s="C:\\NFSU2"; uint32_t l=(uint32_t)strlen(s);
        if(p){ mem_commit(p,l+1); memcpy(g_mem+p,s,l); MR8(p+l)=0; } r=l; }
    else if (!strcmp(n,"SetCurrentDirectoryA")) r=1;
    else if (!strcmp(n,"GetLongPathNameA")||!strcmp(n,"GetShortPathNameA")){
        uint32_t src=A(c,0), dst=A(c,1), sz=A(c,2);
        uint32_t l = src? (uint32_t)strlen((char*)(g_mem+src)) : 0;
        if(dst && sz>l){ memcpy(g_mem+dst, g_mem+src, l+1); }
        r=l; }
    else if (!strcmp(n,"GetSystemDirectoryA")||!strcmp(n,"GetWindowsDirectoryA")){
        uint32_t p=A(c,0); const char*s="C:\\Windows"; uint32_t l=(uint32_t)strlen(s);
        if(p){ mem_commit(p,l+1); memcpy(g_mem+p,s,l); MR8(p+l)=0; } r=l; }
    else if (!strcmp(n,"GetSystemTimeAsFileTime")){
        uint32_t p=A(c,0); if(p){ mem_commit(p,8);
            static uint64_t ft = 130000000000000000ull; ft += 100000;
            MR32(p)=(uint32_t)ft; MR32(p+4)=(uint32_t)(ft>>32); } r=0; }
    else if (!strcmp(n,"GetLocalTime")||!strcmp(n,"GetSystemTime")){
        uint32_t p=A(c,0); if(p){ mem_commit(p,16); memset(g_mem+p,0,16);
            MR16(p)=2026; MR16(p+2)=8; MR16(p+6)=24; MR16(p+8)=12; } r=0; }
    /* --- kopya korumasi / anti-debug: basarisiz don --- */
    else if (!strcmp(n,"CreateToolhelp32Snapshot")) r=0xFFFFFFFFu;
    else if (!strcmp(n,"Process32First")||!strcmp(n,"Process32Next")) r=0;
    else if (!strcmp(n,"IsDebuggerPresent")) r=0;
    /* --- kaynak --- */
    else if (!strcmp(n,"FindResourceA")) r = res_find(A(c,1), A(c,2), 0);
    else if (!strcmp(n,"FindResourceW")) r = res_find(A(c,1), A(c,2), 1);
    else if (!strcmp(n,"LoadResource"))  r = A(c,1);            /* HRSRC'yi tasi */
    else if (!strcmp(n,"LockResource"))  r = res_data(A(c,0));
    else if (!strcmp(n,"SizeofResource"))r = res_size(A(c,1));
    else if (!strcmp(n,"FreeResource"))  r = 1;
    else if (!strcmp(n,"GetSystemInfo")){
        uint32_t p=A(c,0); if(p){ mem_commit(p,36); memset(g_mem+p,0,36);
            MR32(p+4)=4096; MR32(p+20)=10; MR32(p+32)=0x000006d8; } r=0; }
    else if (!strcmp(n,"GlobalMemoryStatus")){
        uint32_t p=A(c,0); if(p){ mem_commit(p,32); memset(g_mem+p,0,32);
            MR32(p)=32; MR32(p+4)=50; MR32(p+8)=0x40000000u; MR32(p+12)=0x20000000u; } r=0; }
    else if (!strcmp(n,"IsBadReadPtr")||!strcmp(n,"IsBadWritePtr")||!strcmp(n,"IsBadCodePtr")) r=0;
    else if (!strcmp(n,"OutputDebugStringA")){ uint32_t p=A(c,0);
        if(p) fprintf(stderr,"[dbg] %s", (char*)(g_mem+p)); r=0; }
    else if (!strcmp(n,"timeBeginPeriod")||!strcmp(n,"timeEndPeriod")) r=0;
    else if (!strcmp(n,"timeGetTime")){ uint32_t host_ms(void); r = host_ms(); }
    else if (!strcmp(n,"timeSetEvent")){
        /* (uDelay, uResolution, lpTimeProc, dwUser, fuEvent) */
        uint32_t delay=A(c,0), proc=A(c,2), user=A(c,3), flags=A(c,4);
        uint32_t id=0;
        static int s_notimer = -1;
        if (s_notimer < 0) s_notimer = getenv("NOTIMER") != 0;
        if (proc && !s_notimer){
            pthread_mutex_lock(&g_lock);
            int timer_ready = 1;
            if (!g_timer_running){
                TimerThreadStart *start = calloc(1,sizeof *start);
                int reserve_rc = start
                    ? win32_guest_thread_resources_reserve(
                          &g_guest_thread_resources, &start->resources)
                    : ENOMEM;
                if (reserve_rc != 0){
                    free(start);
                    timer_ready = 0;
                } else {
                    prepare_guest_thread_resources(&start->resources);
                    g_timer_running = 1;
                    pthread_t th2;
#if TARGET_OS_IPHONE
                    /* Timer callbacks execute lifted guest functions too. */
                    int timer_rc = guest_pthread_create(&th2, timer_thread_main, start);
#else
                    int timer_rc = pthread_create(&th2, 0, timer_thread_main, start);
#endif
                    if (timer_rc == 0) pthread_detach(th2);
                    else {
                        g_timer_running = 0;
                        release_failed_start_locked(&start->resources);
                        free(start);
                        timer_ready = 0;
                    }
                }
            }
            if (timer_ready){
                for (int i=0;i<MAXTIMER;i++){
                    if (g_tm[i].due_us) continue;
                    uint32_t us = delay ? delay*1000u : 1000u;
                    g_tm[i].proc=proc; g_tm[i].user=user;
                    g_tm[i].id = id = g_timer_next_id++;
                    g_tm[i].period_us = (flags & 1u) ? us : 0u;
                    g_tm[i].due_us = now_us() + us;
                    break;
                }
            }
            pthread_mutex_unlock(&g_lock);
            { static int sh2=0; if (sh2<3){ sh2++;
                fprintf(stderr,"  [zamanlayici] timeSetEvent %u ms, %s, geri cagirma 0x%08x\n",
                        delay, (flags&1u)?"periyodik":"tek sefer", proc); } }
        }
        r=id; }
    else if (!strcmp(n,"timeKillEvent")){
        uint32_t id=A(c,0);
        pthread_mutex_lock(&g_lock);
        for (int i=0;i<MAXTIMER;i++)
            if (g_tm[i].due_us && g_tm[i].id==id){ g_tm[i].due_us=0; break; }
        pthread_mutex_unlock(&g_lock);
        r=0; }
    else if (!strcmp(n,"timeGetDevCaps")){
        uint32_t p=A(c,0);
        if(p){ mem_commit(p,8); MR32(p)=1u; MR32(p+4)=1000000u; }
        r=0; }
    else if (!strcmp(n,"timeBeginPeriod")||!strcmp(n,"timeEndPeriod")) r=0;
    else if (!strcmp(n,"joyGetNumDevs")) r=0;
    /* --- GERCEK kritik bolumler ---------------------------------------
     * Etkisiz stub birakilirsa GIL'in adil paylasimi (her 1024 Win32
     * cagrisinda kilidi birakir) karsilikli dislamayi bozuyor: bir thread
     * kritik bolumun icindeyken kilidi kaybedebiliyor.                  */
    else if (!strcmp(n,"InitializeCriticalSection")||
             !strcmp(n,"InitializeCriticalSectionAndSpinCount")){
        (void)cs_for(A(c,0)); r=1; }
    else if (!strcmp(n,"EnterCriticalSection")){
        pthread_mutex_t *m = cs_for(A(c,0));
        if (m){
            g_cs_enter++;
            if (pthread_mutex_trylock(m) != 0){
                g_cs_contended++;
                guest_yield_begin();          /* beklerken GIL'i birak */
                pthread_mutex_lock(m);
                guest_yield_end();
            }
            tl_cs_depth++;
        }
        r=1; }
    else if (!strcmp(n,"TryEnterCriticalSection")){
        pthread_mutex_t *m = cs_for(A(c,0));
        r=1;
        if (m){ if (pthread_mutex_trylock(m)==0) tl_cs_depth++; else r=0; } }
    else if (!strcmp(n,"LeaveCriticalSection")){
        pthread_mutex_t *m = cs_for(A(c,0));
        if (m && tl_cs_depth > 0){ tl_cs_depth--; pthread_mutex_unlock(m); }
        r=1; }
    else if (!strcmp(n,"DeleteCriticalSection")) r=1;
    else return 0;                       /* bu katman ilgilenmiyor */
    *out = r;
    return 1;
}
uint64_t win32_ext_threads(void){ return g_threads_started; }
uint64_t win32_ext_threads_exited(void){ return g_threads_exited; }
