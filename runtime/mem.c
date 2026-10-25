/* Guest bellek: 4 GB rezerve (PROT_NONE), kullanilan bolgeler commit edilir. */
#include <errno.h>
#include <pthread.h>
#include "cpu.h"
#include "guest_memory.h"
#ifndef NFS_FULL_RUNTIME
#define NFS_FULL_RUNTIME 0
#endif
#if NFS_FULL_RUNTIME
#include "guest_pe.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
void (*g_backtrace_fn)(struct CPU*,int) = 0;   /* dispatch.c kurar */
CPU *g_cur_cpu = 0;

uint32_t g_image_size = 0, g_image_entry = 0;
uint32_t g_image_base = 0x400000u, g_res_dir_va = 0, g_res_dir_size = 0;

/* izleme durumu: her derlemede tanimli olmali (debug.c opsiyonel) */
uint32_t g_itrace[ITRACE_SIZE];
uint64_t g_itrace_n = 0;
uint32_t g_watch_addr = 0, g_watch_val = 0, g_watch_prev_ip = 0;
void (*g_itrace_dump_fn)(int) = 0;      /* debug.c kurar */
void (*g_report_fn)(void) = 0;          /* boot.c kurar */
__attribute__((weak)) void watch_report_change(uint32_t ip, uint32_t o, uint32_t n){
    (void)ip;(void)o;(void)n;
}
void mprot_arm(uint32_t);
extern uint8_t *g_mprot_page;
/* Runtime environment variables and page size are startup diagnostics.
 * Parse them once with pthread_once instead of scanning environ on hot paths
 * or signal handlers. */
static pthread_once_t g_env_once = PTHREAD_ONCE_INIT;
static uint32_t g_commit_watch_target;
static int g_commit_watch_enabled, g_commit_watch_done;
static int g_cached_mprot_persist;
static int g_cached_mprot_hard;
static int g_cached_mprot_any;
static int g_cached_itrace_dump;
static int g_cached_itrace_val;
static size_t g_host_page_size;

static void env_cache_init(void){
    const char *wa = getenv("WATCHADDR");
    if (wa){
        g_commit_watch_target = (uint32_t)strtoul(wa, 0, 0);
        g_commit_watch_enabled = 1;
    }
    if (getenv("MPROT_PERSIST")) g_cached_mprot_persist = 1;
    if (getenv("MPROT_HARD")) g_cached_mprot_hard = 1;
    if (getenv("MPROT_ANY")) g_cached_mprot_any = 1;
    const char *it = getenv("ITRACE");
    if (it){
        g_cached_itrace_dump = 1;
        g_cached_itrace_val = atoi(it);
    }
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = getpagesize();
    g_host_page_size = (size_t)ps;
}

static inline int mem_range_all_committed(uint32_t start_pg, uint32_t end_pg){
    if (start_pg + 1 == end_pg){
        return (__atomic_load_n(&g_cbits[start_pg >> 3], __ATOMIC_ACQUIRE) &
                (uint8_t)(1u << (start_pg & 7))) != 0;
    }
    uint32_t p = start_pg;
    while (p < end_pg && (p & 7)){
        if (!((__atomic_load_n(&g_cbits[p >> 3], __ATOMIC_ACQUIRE) >> (p & 7)) & 1))
            return 0;
        p++;
    }
    while (p + 8 <= end_pg){
        if (__atomic_load_n(&g_cbits[p >> 3], __ATOMIC_ACQUIRE) != 0xFFu)
            return 0;
        p += 8;
    }
    while (p < end_pg){
        if (!((__atomic_load_n(&g_cbits[p >> 3], __ATOMIC_ACQUIRE) >> (p & 7)) & 1))
            return 0;
        p++;
    }
    return 1;
}

void mem_init(void){
    pthread_once(&g_env_once, env_cache_init);
    int rc = guest_memory_init();
    if (rc){ errno = rc; perror("mmap 4GB"); exit(1); }
}

void mem_commit(uint32_t addr, uint32_t size){
    if (!size) return;
    /* Preserve the desktop API's legacy clipping at the end of x86 space. */
    uint64_t length = size;
    if (length > GSZ - addr) length = GSZ - addr;

    pthread_once(&g_env_once, env_cache_init);

    /* Safe hot-path fast path: if g_mem is active, no pending watchpoint in range,
     * no active mprotect diagnostic page, and every granule in [addr, addr+length)
     * is already marked committed in g_cbits, return immediately without acquiring
     * locks or repeating mprotect. */
    if (__builtin_expect(g_mem != NULL, 1)){
        uint64_t lo = (uint64_t)addr & ~0x3FFFull;
        uint64_t hi = ((uint64_t)addr + length + 0x3FFFu) & ~0x3FFFull;
        if (hi > GSZ) hi = GSZ;

        int watch_pending = g_commit_watch_enabled &&
            !__atomic_load_n(&g_commit_watch_done, __ATOMIC_RELAXED);
        int mprot_active = (g_mprot_page != NULL);

        if (!watch_pending && !mprot_active){
            uint32_t start_pg = (uint32_t)(lo >> 14);
            uint32_t end_pg = (uint32_t)(hi >> 14);
            if (mem_range_all_committed(start_pg, end_pg))
                return;
        }
    }

    int rc = guest_memory_commit(addr, length);
    if (rc){
        fprintf(stderr,"mem_commit 0x%08x+%u failed: %s\n", addr, size, strerror(rc));
        exit(1);
    }
    /* WATCHADDR=<adres>: o adres commit edilir edilmez watchpoint kur.
     * Boylece ilk yazan bile yakalanir. */
    if (g_commit_watch_enabled &&
        !__atomic_load_n(&g_commit_watch_done,__ATOMIC_RELAXED)){
        uint64_t lo=(uint64_t)addr & ~0x3FFFull;
        uint64_t hi=((uint64_t)addr+length+0x3FFFu) & ~0x3FFFull;
        uint32_t t=g_commit_watch_target;
        if ((uint64_t)t>=lo && (uint64_t)t<hi &&
            !__atomic_exchange_n(&g_commit_watch_done,1,__ATOMIC_RELAXED))
            mprot_arm(t);
    }
}
uint64_t mem_committed(void){ return guest_memory_committed(); }

/* Guest'in commit edilmemis bolgeye erisimini teshis et */
static volatile int g_in_handler = 0;
/* --- yazma yakalama: bir guest sayfasini salt-okunur yapip yazani bul ---
 * Watchpoint enstrumantasyonu olmadan "bu bellege kim yaziyor?" sorusunu
 * cevaplar: yazma SIGSEGV uretir, hatali host PC'den uretilmis fonksiyon
 * (dolayisiyla guest fonksiyonu) cozulur.                                */
uint8_t *g_mprot_page = 0;
static size_t g_mprot_sz = 0;
static int g_mprot_hits = 0;
static uint32_t g_mprot_target = 0;      /* ilgilendigimiz tam adres */
static volatile int g_mprot_found = 0;   /* hedefe yazan bulundu */
static int      g_mprot_persist = 0;     /* her karede yeniden kur */
/* dispatch.c saglar; tek basina linklenen test kosumlarinda zayif varsayilan */
__attribute__((weak)) uint32_t guest_fn_from_host(const void *pc){ (void)pc; return 0; }

/* Korunan sayfa 16 KB ve cok yogun: ilk yabanci yazma korumayi dusuruyor.
 * Ayri bir thread surekli yeniden kurar, boylece hedefe yazan yakalanir. */
static void *mprot_rearm_thread(void *a0){
    (void)a0;
    for (;;){
        /* g_mprot_page kurulu olsa bile yeniden koru: araya giren bir
         * mem_commit korumayi sessizce kaldirmis olabilir. */
        if (g_mprot_target && !g_mprot_found){
            size_t ps = g_host_page_size ? g_host_page_size : (size_t)getpagesize();
            uint8_t *pg = (uint8_t*)((uintptr_t)(g_mem + g_mprot_target) & ~(uintptr_t)(ps-1));
            if (mprotect(pg, ps, PROT_READ) == 0){ g_mprot_page = pg; g_mprot_sz = ps; }
        }
        struct timespec ts = {0, 50 * 1000};    /* 50 us */
        nanosleep(&ts, 0);
    }
    return 0;
}

void mprot_arm(uint32_t guest_addr){
    pthread_once(&g_env_once, env_cache_init);
    /* Apple Silicon'da sayfa 16 KB; hizalama sistem degerinden alinmali. */
    size_t ps = g_host_page_size ? g_host_page_size : (size_t)getpagesize();
    uint8_t *pg = (uint8_t*)((uintptr_t)(g_mem + guest_addr) & ~(uintptr_t)(ps - 1));
    if (mprotect(pg, ps, PROT_READ) != 0){
        fprintf(stderr,"mprot: kurulamadi (%s)\n", strerror(errno)); return;
    }
    g_mprot_page = pg; g_mprot_sz = ps; g_mprot_target = guest_addr;
    g_mprot_persist = g_cached_mprot_persist;
    if (g_cached_mprot_hard){
        static pthread_t th; static int started = 0;
        if (!started){ started = 1; pthread_create(&th, 0, mprot_rearm_thread, 0); }
    }
    fprintf(stderr,"mprot: guest 0x%08x sayfasi salt-okunur (yazani yakalamak icin)\n",
            guest_addr);
}

/* Kare basinda korumayi yeniden kur (tek seferlik yakalama yetmiyorsa). */
void mprot_rearm(void){
    if (!g_mprot_persist || g_mprot_page || !g_mprot_target) return;
    size_t ps = g_host_page_size ? g_host_page_size : (size_t)getpagesize();
    uint8_t *pg = (uint8_t*)((uintptr_t)(g_mem + g_mprot_target) & ~(uintptr_t)(ps - 1));
    if (mprotect(pg, ps, PROT_READ) == 0){ g_mprot_page = pg; g_mprot_sz = ps; }
}

static void segv(int sig, siginfo_t *si, void *uc){
    (void)sig;
    (void)uc; /* The ARM64 diagnostic path below uses the machine context. */
    /* korunan sayfaya yazma mi? */
    if (g_mprot_page){
        uint8_t *fa = (uint8_t*)si->si_addr;
        if (fa >= g_mprot_page && fa < g_mprot_page + g_mprot_sz){
            void *pc = 0;
#if defined(__aarch64__)
            ucontext_t *u = (ucontext_t*)uc;
            if (u && u->uc_mcontext) pc = (void*)(uintptr_t)u->uc_mcontext->__ss.__pc;
#endif
            uint32_t gfn = pc ? guest_fn_from_host(pc) : 0;
            uint32_t ga = (uint32_t)(fa - g_mem);
            int interesting = !g_mprot_target ||
                (ga >= g_mprot_target && ga < g_mprot_target + 64u);
            if (g_cached_mprot_any) interesting = 1;   /* mekanizma testi */
            if (interesting && g_mprot_hits < 12){
                g_mprot_hits++;
                fprintf(stderr,"mprot: HEDEFE YAZMA guest 0x%08x  guest fonksiyon=sub_%08x\n",
                        ga, gfn);
                if (g_mprot_hits >= 6) g_mprot_found = 1;
            }
            mprotect(g_mprot_page, g_mprot_sz, PROT_READ|PROT_WRITE);
            g_mprot_page = 0;           /* tek seferlik */
            return;                      /* komut yeniden calissin */
        }
    }
    if (g_in_handler){ fprintf(stderr,"\n*** isleyici icinde ikinci hata - cikiliyor\n"); _exit(4); }
    g_in_handler = 1;
    /* Hataya neden olan GUEST FONKSIYONU: host PC'den cozulur.  Yigin izi
     * yalnizca cagiranlari verir; hata tam olarak nerede oldu, bu soyler. */
    { void *fpc = 0;
#if defined(__aarch64__)
      ucontext_t *u2 = (ucontext_t*)uc;
      if (u2 && u2->uc_mcontext) fpc = (void*)(uintptr_t)u2->uc_mcontext->__ss.__pc;
#endif
      uint32_t ffn = fpc ? guest_fn_from_host(fpc) : 0;
      if (ffn) fprintf(stderr,"\n*** HATA GUEST FONKSIYONU: sub_%08x\n", ffn);
    }
    uint8_t *p = (uint8_t*)si->si_addr;
    if (p >= g_mem && p < g_mem + GSZ)
        fprintf(stderr,"\n*** GUEST BELLEK HATASI: guest adres 0x%08llx (haritalanmamis)\n",
                (unsigned long long)(p - g_mem));
    else
        fprintf(stderr,"\n*** HOST SEGV: %p (guest alani disi - muhtemelen HOST YIGIN TASMASI:"
                       " guest kodunda sonsuz ozyineleme)\n", p);
    { extern uint32_t g_watch_prev_ip; extern uint64_t g_itrace_n;
      if (g_itrace_n) fprintf(stderr,"    HATAYA NEDEN OLAN GUEST KOMUTU: 0x%08x\n", g_watch_prev_ip); }
    if (g_cur_cpu){
        CPU *c = (CPU*)g_cur_cpu;
        fprintf(stderr,"    eax=%08x ecx=%08x edx=%08x ebx=%08x\n", c->eax,c->ecx,c->edx,c->ebx);
        fprintf(stderr,"    esp=%08x ebp=%08x esi=%08x edi=%08x  df=%u\n", c->esp,c->ebp,c->esi,c->edi,c->df);
        fprintf(stderr,"    fs_base=%08x tid=%u   [ebp+8]=%08x [ebp+4]=%08x\n",
                c->fs_base,
                (c->fs_base && mem_is_committed(c->fs_base+0x24u)) ? MR32(c->fs_base+0x24u) : 0u,
                mem_is_committed(c->ebp+8u) ? MR32(c->ebp+8u) : 0xDEADu,
                mem_is_committed(c->ebp+4u) ? MR32(c->ebp+4u) : 0xDEADu);
    }
    if (g_cur_cpu && g_backtrace_fn) g_backtrace_fn(g_cur_cpu, 12);
    if (g_itrace_dump_fn && g_cached_itrace_dump) g_itrace_dump_fn(g_cached_itrace_val);
    if (g_report_fn) g_report_fn();
    _exit(3);
}
/* sigaltstack THREAD BASINA kurulur: her guest thread'i kendi sinyal
 * yiginini kurmali, yoksa yigin tasmasinda isleyici de coker.          */
void mem_altstack_install(void){
    size_t sz = (size_t)SIGSTKSZ * 8;
    void *p = malloc(sz);
    if (!p) return;
    stack_t ss; ss.ss_sp = p; ss.ss_size = sz; ss.ss_flags = 0;
    sigaltstack(&ss, 0);
}
void mem_fault_handler_install(void){
    mem_altstack_install();
    struct sigaction sa; memset(&sa,0,sizeof sa);
    sa.sa_sigaction = segv; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV,&sa,0); sigaction(SIGBUS,&sa,0);
}

int mem_load_pe(const char *path){
#if NFS_FULL_RUNTIME
    FILE *f;
    long file_size;
    size_t length;
    uint8_t *source;
    int io_error = 0;
    GuestPEImage image;
    char diagnostic[192];

    if (!path){ errno = EINVAL; return -1; }
    f = fopen(path, "rb");
    if (!f) return -1;
    errno = 0;
    if (fseek(f, 0, SEEK_END) != 0){
        io_error = errno ? errno : EIO;
    } else {
        errno = 0;
        file_size = ftell(f);
        if (file_size < 0) io_error = errno ? errno : EIO;
        else if ((uintmax_t)file_size > (uintmax_t)SIZE_MAX) io_error = EFBIG;
        else {
            errno = 0;
            if (fseek(f, 0, SEEK_SET) != 0) io_error = errno ? errno : EIO;
        }
    }
    if (io_error){
        (void)fclose(f);
        errno = io_error;
        return -1;
    }

    length = (size_t)file_size;
    source = (uint8_t *)malloc(length ? length : 1u);
    if (!source){
        io_error = errno ? errno : ENOMEM;
        (void)fclose(f);
        errno = io_error;
        return -1;
    }
    if (length){
        errno = 0;
        if (fread(source, 1, length, f) != length)
            io_error = (ferror(f) && errno) ? errno : EIO;
    }
    errno = 0;
    if (fclose(f) != 0 && !io_error)
        io_error = errno ? errno : EIO;
    if (io_error){
        free(source);
        errno = io_error;
        return -1;
    }

    int rc = guest_pe_load(source, length, &image, diagnostic, sizeof diagnostic);
    free(source);
    if (rc){
        fprintf(stderr, "%s: %s\n", path, diagnostic);
        errno = rc;
        return -1;
    }

    /* Preserve the legacy runtime ABI while sharing the checked VM mapping. */
    g_image_base = image.image_base;
    g_image_size = image.image_size;
    g_image_entry = image.entry_point;
    g_res_dir_va = image.resource_address;
    g_res_dir_size = image.resource_size;
    fprintf(stderr, "  image 0x%08x .. 0x%08x   kaynak dizini 0x%08x (%u bayt), %u bolum\n",
            g_image_base, g_image_base + g_image_size,
            g_res_dir_va, g_res_dir_size, image.section_count);
    return 0;
#else
    FILE *f = fopen(path, "rb");
    if(!f){ perror(path); return -1; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t *buf = malloc(sz);
    if (fread(buf,1,sz,f)!=(size_t)sz){ fclose(f); free(buf); return -1; }
    fclose(f);
    uint32_t e_lfanew = *(uint32_t*)(buf+0x3c);
    uint8_t *nt = buf + e_lfanew;
    if (*(uint32_t*)nt != 0x00004550u){ fprintf(stderr,"PE imzasi yok\n"); free(buf); return -1; }
    uint16_t nsec  = *(uint16_t*)(nt+6);
    uint16_t optsz = *(uint16_t*)(nt+20);
    uint32_t base  = *(uint32_t*)(nt+24+28);
    uint32_t imgsz = *(uint32_t*)(nt+24+56);
    g_image_size = imgsz; g_image_entry = base + *(uint32_t*)(nt+24+16);
    /* DataDirectory[2] = RESOURCE */
    { /* PE32: DataDirectory opsiyonel basligin 96. baytinda; RESOURCE = indeks 2 */
      uint32_t dd = 24 + 96 + 2*8;
      uint32_t rva = *(uint32_t*)(nt + dd);
      uint32_t sz  = *(uint32_t*)(nt + dd + 4);
      g_image_base = base; g_res_dir_va = rva ? base + rva : 0; g_res_dir_size = sz; }
    uint8_t *sh    = nt + 24 + optsz;
    mem_init();
    mem_commit(base, imgsz);                 /* tum image araligi */
    for (int i=0;i<nsec;i++){
        uint8_t *s = sh + i*40;
        char nm[9]={0}; memcpy(nm,s,8);
        uint32_t vsize=*(uint32_t*)(s+8), va=*(uint32_t*)(s+12);
        uint32_t rsize=*(uint32_t*)(s+16), roff=*(uint32_t*)(s+20);
        uint32_t n = rsize < vsize ? rsize : vsize;
        memcpy(g_mem + base + va, buf + roff, n);
        fprintf(stderr,"  yuklendi %-9s -> 0x%08x  %u bayt\n", nm[0]?nm:"<isimsiz>", base+va, n);
    }
    free(buf);
    fprintf(stderr,"  image 0x%08x .. 0x%08x   kaynak dizini 0x%08x (%u bayt)\n",
            base, base+imgsz, g_res_dir_va, g_res_dir_size);
    return 0;
#endif
}
