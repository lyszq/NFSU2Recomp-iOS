/* Guest dosya G/C -> POSIX.  Windows yollarini oyun dizinine esler,
 * buyuk/kucuk harf duyarsiz cozumleme yapar.                          */
#include "cpu.h"
#include "guest_memory.h"
#include "fileio.h"
#include "win32_full_path.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
static uint64_t now_ns_io(void){
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC,&ts) != 0) return 0;
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
}

void mem_commit(uint32_t,uint32_t);
void win32_set_last_error(CPU *, uint32_t);

static char g_root[1024] = ".";
static char g_cwd_rel[512] = "";           /* oyun icindeki gecerli dizin */
uint64_t g_file_open=0, g_file_fail=0, g_file_read=0, g_bytes_read=0;
uint64_t g_overlapped_reads=0;
static int g_empty_next = 0;

/* Directory-component results are only useful when the directory itself has
 * not changed. Keep a bounded cache and validate every hit against the
 * parent's identity and nanosecond timestamps. Files can still be added or
 * removed by the game, another thread, or the user while a session runs. */
#define FILEIO_CASE_PARENT_CAP 1024u
#define FILEIO_CASE_NAME_CAP 256u
#define FILEIO_CASE_ACTUAL_CAP 1024u
typedef struct {
    int valid;
    char parent[FILEIO_CASE_PARENT_CAP];
    char folded_name[FILEIO_CASE_NAME_CAP];
    char actual_name[FILEIO_CASE_NAME_CAP];
    int found;
    dev_t dev;
    ino_t ino;
    int64_t mtime_sec;
    long mtime_nsec;
    int64_t ctime_sec;
    long ctime_nsec;
    uint64_t age;
} FileioCaseEntry;

static pthread_mutex_t g_fileio_case_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static FileioCaseEntry g_fileio_case_cache[FILEIO_CASE_CACHE_CAPACITY];
static uint64_t g_fileio_case_cache_age;
static _Atomic uint64_t g_fileio_case_cache_scans;
static _Atomic uint64_t g_fileio_case_cache_hits;
static _Atomic uint64_t g_fileio_case_cache_invalidations;
static _Atomic uint64_t g_fileio_snapshot_read_calls;
static _Atomic uint64_t g_fileio_snapshot_read_bytes;
static _Atomic uint64_t g_fileio_read_commit_ns;
static _Atomic uint64_t g_fileio_read_service_ns;
static _Atomic uint64_t g_fileio_read_service_max_ns;

#if defined(__APPLE__)
#define FILEIO_STAT_MTIME_SEC(st_) ((st_)->st_mtimespec.tv_sec)
#define FILEIO_STAT_MTIME_NSEC(st_) ((st_)->st_mtimespec.tv_nsec)
#define FILEIO_STAT_CTIME_SEC(st_) ((st_)->st_ctimespec.tv_sec)
#define FILEIO_STAT_CTIME_NSEC(st_) ((st_)->st_ctimespec.tv_nsec)
#else
#define FILEIO_STAT_MTIME_SEC(st_) ((st_)->st_mtim.tv_sec)
#define FILEIO_STAT_MTIME_NSEC(st_) ((st_)->st_mtim.tv_nsec)
#define FILEIO_STAT_CTIME_SEC(st_) ((st_)->st_ctim.tv_sec)
#define FILEIO_STAT_CTIME_NSEC(st_) ((st_)->st_ctim.tv_nsec)
#endif

static int fileio_case_fold(const char *name, char folded[FILEIO_CASE_NAME_CAP]){
    size_t length = strlen(name);
    if (!length || length >= FILEIO_CASE_NAME_CAP) return 0;
    for (size_t i = 0; i < length; i++)
        folded[i] = (char)tolower((unsigned char)name[i]);
    folded[length] = '\0';
    return 1;
}

static int fileio_case_stat_matches(const FileioCaseEntry *entry,
                                    const struct stat *st){
    return entry->dev == st->st_dev && entry->ino == st->st_ino &&
           entry->mtime_sec == (int64_t)FILEIO_STAT_MTIME_SEC(st) &&
           entry->mtime_nsec == (long)FILEIO_STAT_MTIME_NSEC(st) &&
           entry->ctime_sec == (int64_t)FILEIO_STAT_CTIME_SEC(st) &&
           entry->ctime_nsec == (long)FILEIO_STAT_CTIME_NSEC(st);
}

static void fileio_case_cache_store(const char *parent, const char *folded,
                                    const char *actual, int found,
                                    const struct stat *st){
    if (strlen(parent) >= FILEIO_CASE_PARENT_CAP) return;
    if (found && (!actual || strlen(actual) >= FILEIO_CASE_NAME_CAP)) return;

    pthread_mutex_lock(&g_fileio_case_cache_lock);
    size_t slot = FILEIO_CASE_CACHE_CAPACITY;
    size_t first_free = FILEIO_CASE_CACHE_CAPACITY;
    size_t oldest = FILEIO_CASE_CACHE_CAPACITY;
    uint64_t oldest_age = UINT64_MAX;
    for (size_t i = 0; i < FILEIO_CASE_CACHE_CAPACITY; i++){
        FileioCaseEntry *entry = &g_fileio_case_cache[i];
        if (entry->valid && strcmp(entry->parent, parent) == 0 &&
            strcmp(entry->folded_name, folded) == 0){
            slot = i;
            break;
        }
        if (!entry->valid){
            if (first_free == FILEIO_CASE_CACHE_CAPACITY) first_free = i;
            continue;
        }
        if (entry->age < oldest_age){ oldest_age = entry->age; oldest = i; }
    }
    if (slot == FILEIO_CASE_CACHE_CAPACITY)
        slot = first_free < FILEIO_CASE_CACHE_CAPACITY ? first_free : oldest;
    if (slot < FILEIO_CASE_CACHE_CAPACITY){
        FileioCaseEntry *entry = &g_fileio_case_cache[slot];
        memset(entry, 0, sizeof(*entry));
        snprintf(entry->parent, sizeof(entry->parent), "%s", parent);
        snprintf(entry->folded_name, sizeof(entry->folded_name), "%s", folded);
        if (found) snprintf(entry->actual_name, sizeof(entry->actual_name), "%s", actual);
        entry->found = found;
        entry->dev = st->st_dev;
        entry->ino = st->st_ino;
        entry->mtime_sec = (int64_t)FILEIO_STAT_MTIME_SEC(st);
        entry->mtime_nsec = (long)FILEIO_STAT_MTIME_NSEC(st);
        entry->ctime_sec = (int64_t)FILEIO_STAT_CTIME_SEC(st);
        entry->ctime_nsec = (long)FILEIO_STAT_CTIME_NSEC(st);
        entry->age = ++g_fileio_case_cache_age;
        entry->valid = 1;
    }
    pthread_mutex_unlock(&g_fileio_case_cache_lock);
}

/* Return 1 when a cached or freshly scanned answer is available, 0 when the
 * caller should use its uncached fallback, and -1 when the parent could not
 * be opened. Filesystem work stays outside the cache lock. */
static int fileio_case_cache_resolve(const char *parent, const char *wanted,
                                     char *actual, size_t actual_size,
                                     int *out_found){
    char folded[FILEIO_CASE_NAME_CAP];
    int cacheable = strlen(parent) < FILEIO_CASE_PARENT_CAP &&
                    fileio_case_fold(wanted, folded);
    struct stat before;
    if (stat(parent, &before) != 0) return 0;

    if (cacheable){
        pthread_mutex_lock(&g_fileio_case_cache_lock);
        for (size_t i = 0; i < FILEIO_CASE_CACHE_CAPACITY; i++){
            FileioCaseEntry *entry = &g_fileio_case_cache[i];
            if (!entry->valid || strcmp(entry->parent, parent) != 0 ||
                strcmp(entry->folded_name, folded) != 0) continue;
            if (fileio_case_stat_matches(entry, &before)){
                entry->age = ++g_fileio_case_cache_age;
                *out_found = entry->found;
                if (entry->found)
                    snprintf(actual, actual_size, "%s", entry->actual_name);
                pthread_mutex_unlock(&g_fileio_case_cache_lock);
                atomic_fetch_add_explicit(&g_fileio_case_cache_hits, 1u,
                                          memory_order_relaxed);
                return 1;
            }
            entry->valid = 0;
            atomic_fetch_add_explicit(&g_fileio_case_cache_invalidations, 1u,
                                      memory_order_relaxed);
            break;
        }
        pthread_mutex_unlock(&g_fileio_case_cache_lock);
    }

    DIR *directory = opendir(parent);
    if (!directory) return -1;
    atomic_fetch_add_explicit(&g_fileio_case_cache_scans, 1u,
                              memory_order_relaxed);
    int found = 0;
    char matched[FILEIO_CASE_ACTUAL_CAP] = "";
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL){
        if (strcasecmp(entry->d_name, wanted) == 0){
            size_t length = strlen(entry->d_name);
            if (length < sizeof(matched)){
                memcpy(matched, entry->d_name, length + 1u);
                found = 1;
            }
            break;
        }
    }
    closedir(directory);

    struct stat after;
    if (cacheable && stat(parent, &after) == 0 &&
        before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
        FILEIO_STAT_MTIME_SEC(&before) == FILEIO_STAT_MTIME_SEC(&after) &&
        FILEIO_STAT_MTIME_NSEC(&before) == FILEIO_STAT_MTIME_NSEC(&after) &&
        FILEIO_STAT_CTIME_SEC(&before) == FILEIO_STAT_CTIME_SEC(&after) &&
        FILEIO_STAT_CTIME_NSEC(&before) == FILEIO_STAT_CTIME_NSEC(&after))
        fileio_case_cache_store(parent, folded, matched, found, &after);

    *out_found = found;
    if (found) snprintf(actual, actual_size, "%s", matched);
    return 1;
}

static void fileio_case_cache_clear(void){
    pthread_mutex_lock(&g_fileio_case_cache_lock);
    uint64_t invalidated = 0;
    for (size_t i = 0; i < FILEIO_CASE_CACHE_CAPACITY; i++)
        if (g_fileio_case_cache[i].valid) invalidated++;
    memset(g_fileio_case_cache, 0, sizeof(g_fileio_case_cache));
    pthread_mutex_unlock(&g_fileio_case_cache_lock);
    atomic_fetch_add_explicit(&g_fileio_case_cache_invalidations, invalidated,
                              memory_order_relaxed);
}

static void fileio_case_cache_invalidate_parent(const char *parent){
    uint64_t invalidated = 0;
    pthread_mutex_lock(&g_fileio_case_cache_lock);
    for (size_t i = 0; i < FILEIO_CASE_CACHE_CAPACITY; i++)
        if (g_fileio_case_cache[i].valid &&
            strcmp(g_fileio_case_cache[i].parent, parent) == 0){
            g_fileio_case_cache[i].valid = 0;
            invalidated++;
        }
    pthread_mutex_unlock(&g_fileio_case_cache_lock);
    atomic_fetch_add_explicit(&g_fileio_case_cache_invalidations, invalidated,
                              memory_order_relaxed);
}

static void fileio_case_cache_invalidate_path(const char *path){
    char parent[FILEIO_CASE_PARENT_CAP];
    size_t length = strlen(path);
    if (!length || length >= sizeof(parent)) return;
    memcpy(parent, path, length + 1u);
    char *slash = strrchr(parent, '/');
    if (!slash) return;
    if (slash == parent) slash[1] = '\0';
    else *slash = '\0';
    fileio_case_cache_invalidate_parent(parent);
}

/* Read-side diagnostics are fixed for the lifetime of the process. Resolve
 * the mute filter once instead of calling getenv (which takes a libc lock)
 * for each file-read chunk. */
typedef struct {
    char *file_filter;
    long exact_size;
    long minimum_size;
    int enabled;
    int readlog;
} FileioMuteOptions;
static pthread_once_t g_fileio_mute_once = PTHREAD_ONCE_INIT;
static FileioMuteOptions g_fileio_mute_options;

/* Launch-time diagnostic environment is treated as immutable after the first
 * file read. Keep a process-lifetime copy of the full configured filter. */
static void fileio_load_mute_options(void){
    const char *file_filter = getenv("MUTEFILE");
    const char *exact_size = getenv("MUTESIZE");
    const char *minimum_size = getenv("MUTEMIN");
    g_fileio_mute_options.readlog = getenv("READLOG") != NULL;
    if (file_filter && *file_filter){
        size_t length = strlen(file_filter);
        g_fileio_mute_options.file_filter = malloc(length + 1u);
        if (g_fileio_mute_options.file_filter){
            memcpy(g_fileio_mute_options.file_filter, file_filter, length + 1u);
            g_fileio_mute_options.enabled = 1;
        }
    }
    g_fileio_mute_options.exact_size = exact_size ? atol(exact_size) : 0;
    g_fileio_mute_options.minimum_size = minimum_size ? atol(minimum_size) : 0;
}

static int fileio_should_mute_read(const char *path, uint32_t requested,
                                   ssize_t bytes_read){
    if (bytes_read <= 0 || !g_fileio_mute_options.enabled ||
        !strstr(path, g_fileio_mute_options.file_filter)) return 0;
    if (g_fileio_mute_options.minimum_size)
        return (long)requested >= g_fileio_mute_options.minimum_size;
    return !g_fileio_mute_options.exact_size ||
           (long)requested == g_fileio_mute_options.exact_size;
}

static int fileio_readlog_enabled(void){
    pthread_once(&g_fileio_mute_once, fileio_load_mute_options);
    return g_fileio_mute_options.readlog;
}

void fileio_perf_snapshot(FileioPerfSnapshot *snapshot){
    if (!snapshot) return;
    snapshot->case_cache_hits = atomic_load_explicit(&g_fileio_case_cache_hits,
                                                    memory_order_relaxed);
    snapshot->directory_scans = atomic_load_explicit(&g_fileio_case_cache_scans,
                                                      memory_order_relaxed);
    snapshot->cache_invalidations = atomic_load_explicit(
        &g_fileio_case_cache_invalidations, memory_order_relaxed);
    snapshot->read_calls = atomic_load_explicit(&g_fileio_snapshot_read_calls,
                                                memory_order_relaxed);
    snapshot->read_bytes = atomic_load_explicit(&g_fileio_snapshot_read_bytes,
                                                memory_order_relaxed);
    snapshot->read_commit_ns = atomic_load_explicit(&g_fileio_read_commit_ns,
                                                    memory_order_relaxed);
    snapshot->read_service_ns = atomic_load_explicit(&g_fileio_read_service_ns,
                                                     memory_order_relaxed);
    snapshot->read_service_max_ns = atomic_load_explicit(&g_fileio_read_service_max_ns,
                                                         memory_order_relaxed);
}

void fileio_set_root(const char *exe_path){
    strncpy(g_root, exe_path, sizeof g_root - 1);
    char *slash = strrchr(g_root, '/');
    if (slash) *slash = 0; else strcpy(g_root, ".");
    fileio_case_cache_clear();
    fprintf(stderr,"dosya: oyun kok dizini = %s\n", g_root);
}

/* Windows yolunu gercek yola cevir.
 *   "C:\\NFSU2\\TRACKS\\X.BUN" | "TRACKS\\X.BUN" | ".\\X" -> <kok>/TRACKS/X.BUN
 * Sanal oyun dizini "C:\\NFSU2" oldugu icin o onek atilir.
 * Bilesenler buyuk-kucuk harf duyarsiz cozumlenir.                     */
/* Yazma amacli acilislarda eksik ust dizinleri olustur (Windows'ta oyun
 * bunlari ayrica yaratiyor; bizde yolun tamami hazir olmali). */
static void mkparents(const char *path){
    char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *q = tmp + 1; *q; q++){
        if (*q != '/') continue;
        *q = 0;
        if (mkdir(tmp, 0755) == 0) fileio_case_cache_invalidate_path(tmp);
        *q = '/';
    }
}

static int resolve_in(const char *root, const char *win, char *out, size_t outsz){
    char rel[1024]; size_t k=0;
    const char *p = win;
    if (p[0] && p[1]==':') p += 2;                       /* surucu harfi   */
    while (*p=='\\' || *p=='/') p++;
    if (!strncasecmp(p,"NFSU2",5) && (p[5]=='\\'||p[5]=='/'||p[5]==0)){
        p += 5; while (*p=='\\'||*p=='/') p++;           /* sanal kok      */
    }
    while (p[0]=='.' && (p[1]=='\\'||p[1]=='/')) p += 2;
    for (; *p && k<sizeof rel-1; p++) rel[k++] = (*p=='\\') ? '/' : *p;
    rel[k]=0;
    if (!rel[0]){ snprintf(out,outsz,"%s",root); return 1; }

    snprintf(out, outsz, "%s/%s", root, rel);
    struct stat st;
    if (stat(out, &st)==0) return 1;

    /* bilesen bilesen buyuk-kucuk harf duyarsiz */
    char cur[1024]; snprintf(cur, sizeof cur, "%s", root);
    char tmp[1024]; snprintf(tmp,sizeof tmp,"%s",rel);
    char *save=0; int missing=0;
    for (char *tok=strtok_r(tmp,"/",&save); tok; tok=strtok_r(0,"/",&save)){
        if (missing){ size_t l=strlen(cur); snprintf(cur+l,sizeof cur-l,"/%s",tok); continue; }
        char cand[1024]; snprintf(cand,sizeof cand,"%s/%s",cur,tok);
        if (stat(cand,&st)==0){ snprintf(cur,sizeof cur,"%s",cand); continue; }
        int found=0;
        char actual[FILEIO_CASE_ACTUAL_CAP];
        int case_result = fileio_case_cache_resolve(cur, tok, actual,
                                                    sizeof actual, &found);
        if (case_result == 0){
            DIR *d = opendir(cur);
            if (d){
                atomic_fetch_add_explicit(&g_fileio_case_cache_scans, 1u,
                                          memory_order_relaxed);
                struct dirent *e;
                while ((e=readdir(d))){
                    if (!strcasecmp(e->d_name, tok)){
                        snprintf(actual,sizeof actual,"%s",e->d_name);
                        found=1; break; } }
                closedir(d); }
        }
        if (found){
            int candidate_length = snprintf(cand, sizeof cand, "%s/%s", cur, actual);
            if (candidate_length < 0 || (size_t)candidate_length >= sizeof(cand))
                found = 0;
        }
        if (found) snprintf(cur,sizeof cur,"%s",cand);
        else { snprintf(cand,sizeof cand,"%s/%s",cur,tok);
               snprintf(cur,sizeof cur,"%s",cand); missing=1; }
    }
    snprintf(out,outsz,"%s",cur);
    return !missing;
}

/* --- yazma koku (kayit katmani) ----------------------------------------
 * Oyun kayitlarini ve profillerini KENDI klasorune yazar.  Veri .app'in
 * icine gomuluyken oraya yazmak paketin imzasini bozar; ayrica uygulama
 * guncellenince kayitlar silinir.  Yazma koku ayarliysa:
 *   - yazma / olusturma / silme her zaman yazma kokune gider,
 *   - okuma once yazma kokune, yoksa oyun kokune bakar,
 *   - dizin listesi ikisinin birlesimidir.
 * Ayarli degilse (terminalden ya da verisi disarida olan paket) her sey
 * eskisi gibi tek kokte kalir. */
static char g_wroot[1024] = "";
void fileio_set_write_root(const char *dir){
    if (!dir || !*dir){ g_wroot[0] = 0; fileio_case_cache_clear(); return; }
    snprintf(g_wroot, sizeof g_wroot, "%s", dir);
    mkparents(g_wroot); mkdir(g_wroot, 0755);
    fileio_case_cache_clear();
    fprintf(stderr,"dosya: kayit dizini = %s\n", g_wroot);
}
/* Okuma: yazma kokunde varsa orasi, yoksa oyun koku. */
static int resolve(const char *win, char *out, size_t outsz){
    if (g_wroot[0]){
        char w[1024];
        if (resolve_in(g_wroot, win, w, sizeof w) && strcmp(w, g_wroot) != 0){
            snprintf(out, outsz, "%s", w); return 1; }
    }
    return resolve_in(g_root, win, out, outsz);
}
/* Yazma: yazma koku varsa HER ZAMAN orasi. */
static int resolve_w(const char *win, char *out, size_t outsz){
    return resolve_in(g_wroot[0] ? g_wroot : g_root, win, out, outsz);
}
static int copy_file(const char *a, const char *b){
    FILE *fa = fopen(a, "rb"); if (!fa) return 0;
    mkparents(b);
    fileio_case_cache_invalidate_path(b);
    FILE *fb = fopen(b, "wb"); if (!fb){ fclose(fa); return 0; }
    char bufr[65536]; size_t k;
    while ((k = fread(bufr, 1, sizeof bufr, fa)) > 0) fwrite(bufr, 1, k, fb);
    fclose(fb); fclose(fa); return 1;
}
/* Kirpmadan yazilacak bir dosya yalnizca oyun kokunde duruyorsa once
 * yazma kokune kopyala; yoksa oyun bos bir dosyanin uzerine yazar. */
static void copy_up(const char *win, const char *wpath){
    if (!g_wroot[0]) return;
    struct stat st; if (stat(wpath, &st) == 0) return;
    char src[1024];
    if (resolve_in(g_root, win, src, sizeof src) && stat(src, &st) == 0 && S_ISREG(st.st_mode))
        copy_file(src, wpath);
}

/* --- handle tablosu --- */
#define FH_BASE 0x00900000u
#define FH_MAX  256
typedef struct { int fd; int empty; int used; char path[512]; DIR *dir; char pat[128];
                 DIR *dir2; char path1[512]; } FH;   /* dir2: oyun koku (kayit katmani etkinken) */
static FH g_fh[FH_MAX];
static uint32_t fh_new(void){
    for (int i=1;i<FH_MAX;i++) if(!g_fh[i].used){ memset(&g_fh[i],0,sizeof g_fh[i]);
        g_fh[i].used=1; g_fh[i].fd=-1; return FH_BASE+(uint32_t)i; }
    return 0;
}
static FH *fh_get(uint32_t h){
    if (h<FH_BASE || h>=FH_BASE+FH_MAX) return 0;
    FH *f=&g_fh[h-FH_BASE]; return f->used?f:0;
}
int fileio_is_handle(uint32_t h){ return fh_get(h)!=0; }

static uint32_t A(CPU *c,int i){ return MR32(c->esp + 4u*(uint32_t)i); }
#define GS(a) ((const char*)(g_mem + (uint32_t)(a)))
#define INVALID_HANDLE 0xFFFFFFFFu

/* Dizin taramasinda siradaki adi ver: once yazma koku, sonra oyun koku. */
static int find_next(FH *f, char *nm, size_t nsz){
    struct dirent *e;
    if (f->dir){
        while ((e = readdir(f->dir))){
            if (e->d_name[0] == '.') continue;
            snprintf(nm, nsz, "%s", e->d_name); return 1; }
    }
    if (f->dir2){
        while ((e = readdir(f->dir2))){
            if (e->d_name[0] == '.') continue;
            if (f->dir && f->path1[0]){     /* yazma kokunde zaten verildi */
                char p[1024]; struct stat st;
                snprintf(p, sizeof p, "%s/%s", f->path1, e->d_name);
                if (stat(p, &st) == 0) continue; }
            snprintf(nm, nsz, "%s", e->d_name); return 1; }
    }
    return 0;
}

/* Read a guest ANSI string without touching uncommitted guest memory. Check
 * at 16 KiB granularity, then scan only the committed chunk for its NUL. */
static int full_path_read_guest_string(uint32_t address, char *buffer,
                                       size_t capacity, uint32_t *error){
    size_t copied = 0;
    if (!address || !buffer || !capacity){
        *error = WIN32_FULL_PATH_ERROR_INVALID_PARAMETER;
        return 0;
    }
    while (copied < capacity){
        uint64_t current = (uint64_t)address + copied;
        if (current > UINT32_MAX){
            *error = WIN32_FULL_PATH_ERROR_NOACCESS;
            return 0;
        }
        size_t within = (size_t)((uint32_t)current & (GUEST_MEMORY_GRANULE - 1u));
        size_t chunk = GUEST_MEMORY_GRANULE - within;
        if (chunk > capacity - copied) chunk = capacity - copied;
        const char *guest = (const char *)guest_memory_pointer((uint32_t)current,
                                                               chunk, 0);
        if (!guest){
            *error = WIN32_FULL_PATH_ERROR_NOACCESS;
            return 0;
        }
        const char *nul = (const char *)memchr(guest, '\0', chunk);
        size_t bytes = nul ? (size_t)(nul - guest) : chunk;
        memcpy(buffer + copied, guest, bytes);
        copied += bytes;
        if (nul){
            buffer[copied] = '\0';
            return 1;
        }
    }
    *error = WIN32_FULL_PATH_ERROR_FILENAME_EXCED_RANGE;
    return 0;
}

/* GetFullPathNameA contract: success returns path bytes excluding NUL;
 * undersized buffers return required bytes including NUL. The path namespace
 * is lexical and guest-only; it never resolves against the host filesystem. */
static uint32_t fileio_get_full_path_name_a(CPU *c){
    uint32_t arguments[4];
    uint32_t error = WIN32_FULL_PATH_ERROR_INVALID_PARAMETER;
    uint32_t input_address, capacity, output_address, file_part_address;
    uint8_t *frame;
    char path[WIN32_FULL_PATH_STORAGE];
    Win32FullPathInfo info;

    if (!c){
        return 0;
    }
    frame = (uint8_t *)guest_memory_pointer(c->esp, sizeof arguments, 0);
    if (!frame){
        win32_set_last_error(c, WIN32_FULL_PATH_ERROR_NOACCESS);
        return 0;
    }
    memcpy(arguments, frame, sizeof arguments);
    input_address = arguments[0];
    capacity = arguments[1];
    output_address = arguments[2];
    file_part_address = arguments[3];

    if (!input_address){
        win32_set_last_error(c, WIN32_FULL_PATH_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!full_path_read_guest_string(input_address, path, sizeof path, &error)){
        win32_set_last_error(c, error);
        return 0;
    }
    if (!win32_full_path_normalize(path, path, &info, &error)){
        win32_set_last_error(c, error);
        return 0;
    }

    uint32_t required = (uint32_t)(info.length + 1u);
    if (capacity < required){
        return required;
    }
    if (!output_address){
        win32_set_last_error(c, WIN32_FULL_PATH_ERROR_INVALID_PARAMETER);
        return 0;
    }

    char *output = (char *)guest_memory_pointer(output_address, required, 1);
    uint32_t *file_part_output = NULL;
    if (!output){
        win32_set_last_error(c, WIN32_FULL_PATH_ERROR_NOACCESS);
        return 0;
    }
    if (file_part_address){
        file_part_output = (uint32_t *)guest_memory_pointer(file_part_address,
                                                             sizeof(uint32_t), 1);
        if (!file_part_output){
            win32_set_last_error(c, WIN32_FULL_PATH_ERROR_NOACCESS);
            return 0;
        }
    }

    memcpy(output, path, required);
    if (file_part_output){
        uint32_t file_part = info.has_file_part
            ? output_address + (uint32_t)info.file_part_offset : 0u;
        memcpy(file_part_output, &file_part, sizeof file_part);
    }
    return (uint32_t)info.length;
}

int fileio_call(CPU *c, const char *n, uint32_t *out){
    uint32_t r = 0;
    if (!strcmp(n,"GetFullPathNameA")){
        r = fileio_get_full_path_name_a(c);
    }
    else if (!strcmp(n,"CreateFileA")){
        uint32_t pn=A(c,0), access=A(c,1), disp=A(c,4);
        char real[1024];
        int flags;
        if (disp==2 /*CREATE_ALWAYS*/ || disp==1 /*CREATE_NEW*/) flags=O_RDWR|O_CREAT|O_TRUNC;
        else if (disp==4 /*OPEN_ALWAYS*/) flags=O_RDWR|O_CREAT;
        else flags = (access & 0x40000000u) ? O_RDWR : O_RDONLY;
        int ok;
        if (flags != O_RDONLY){
            ok = resolve_w(GS(pn), real, sizeof real);
            if (!(flags & O_TRUNC)) copy_up(GS(pn), real);
        } else ok = resolve(GS(pn), real, sizeof real);
        if (flags & O_CREAT){
            mkparents(real);
            fileio_case_cache_invalidate_path(real);
        }
        /* Dil eki geri dusumu: bu surumde film yolunda "_fr.vp6" soneki
         * gomulu ama klasorde "_en" dosyalari var.  Istenen yoksa
         * "_en" ve eksiz surumu dene.                                   */
        struct stat _st;
        static int s_file_log = -1;
        if (s_file_log < 0) s_file_log = (getenv("VERBOSE") != 0 || getenv("FILELIST") != 0);
        if (!(flags & O_CREAT) && !ok){
            char alt[1024]; const char *dot = strrchr(real, '.');
            if (dot && dot - real > 3){
                size_t base = (size_t)(dot - real);
                if (base >= 3 && real[base-3] == '_'){
                    size_t cut = base - 3;
                    const char *sfx[] = { "_en", "", "_dn", "_du", "_sw" };
                    for (int q = 0; q < 5; q++){
                        snprintf(alt, sizeof alt, "%.*s%s%s",
                                 (int)cut, real, sfx[q], dot);
                        if (stat(alt, &_st) == 0){
                            if (s_file_log)
                                fprintf(stderr,"  [dosya] dil geri dusumu: \"%s\" -> \"%s\"\n",
                                        real, alt);
                            snprintf(real, sizeof real, "%s", alt);
                            break;
                        }
                    }
                }
            }
        }
        /* NOMOVIES=1: video dosyalarini yok say.  VP6 kod cozucusu yok;
         * oyun videoyu oynatmaya calisip beklerse orada takiliyor. */
        int fd;
        { const char *ext = strrchr(real, '.');
          static int s_movie_mode = -1;
          if (s_movie_mode < 0){
              const char *nm2 = getenv("NOMOVIES");
              s_movie_mode = (nm2 && *nm2 && *nm2 != '0') ? atoi(nm2) : 0;
          }
          int movie_mode = s_movie_mode;
          /* mod 3: yalnizca "blank" filmini atla (gercek filmler oynasin) */
          const char *bn2 = strrchr(real, '/'); bn2 = bn2 ? bn2+1 : real;
          if (movie_mode == 3 && !strncasecmp(bn2, "blank", 5)) fd = -1;
          else if (movie_mode == 1 && ext &&
              (!strcasecmp(ext,".vp6") || !strcasecmp(ext,".vp8") ||
               !strcasecmp(ext,".bik") || !strcasecmp(ext,".mpg")))
              fd = -1;
          else if (movie_mode == 3 && !strncasecmp(bn2, "blank", 5)) { }
          else {
              fd = open(real, flags, 0644);
#ifdef F_RDAHEAD
              if (fd >= 0 && flags == O_RDONLY) (void)fcntl(fd, F_RDAHEAD, 1);
#endif
              /* mod 2: acilsin ama bos gorunsun -> oynatici "bitti" desin */
              if (fd >= 0 && movie_mode == 2 && ext &&
                  (!strcasecmp(ext,".vp6") || !strcasecmp(ext,".vp8") ||
                   !strcasecmp(ext,".bik") || !strcasecmp(ext,".mpg")))
                  g_empty_next = 1;
          } }
        { static int shown=0; if (s_file_log && shown<200){ shown++;
            fprintf(stderr,"  [dosya] CreateFileA(\"%s\") -> \"%s\"  %s\n",
                    GS(pn), real, fd>=0?"ACIK":"BASARISIZ"); } }
        if (fd<0){ g_file_fail++; r=INVALID_HANDLE;
            fprintf(stderr,"  [dosya] ACILAMADI: \"%s\" -> \"%s\" (disp=%u erisim=0x%x)\n",
                    GS(pn), real, disp, access); }
        else {
            uint32_t h=fh_new();
            if(!h){ close(fd); r=INVALID_HANDLE; }
            else { FH*f=fh_get(h); f->fd=fd; f->empty = g_empty_next; g_empty_next = 0;
                   snprintf(f->path,sizeof f->path,"%s",real);
                   g_file_open++; r=h; }
        }
    }
    else if (!strcmp(n,"ReadFile")){
        /* (hFile, lpBuffer, nBytes, lpNumberRead, lpOverlapped)
         * OVERLAPPED verilirse: ofset yapidan alinir, sonuc yapiya yazilir
         * ve hEvent sinyallenir.  Bunlar yapilmazsa oyun asenkron okumanin
         * bitmesini sonsuza kadar bekler.                               */
        FH*f=fh_get(A(c,0)); uint32_t buf=A(c,1), len=A(c,2), pnr=A(c,3), ov=A(c,4);
        if(!f||f->fd<0){ if(pnr) MR32(pnr)=0; r=0; }
        else { /* Initialize the immutable diagnostic options before timing. */
               int _readlog = fileio_readlog_enabled();
               uint64_t _commit0 = now_ns_io();
               if(len) mem_commit(buf,len);
               ssize_t got;
               /* Bounded counters replace per-read log spam for attribution.
                * Three monotonic reads per IO, never a scan/copy of file data. */
               uint64_t _rd0 = now_ns_io();
               if (ov && mem_is_committed(ov+19)){
                   uint64_t off = (uint64_t)MR32(ov+8) | ((uint64_t)MR32(ov+12) << 32);
                   got = pread(f->fd, g_mem+buf, len, (off_t)off);
                   if(got<0) got=0;
                   MR32(ov+0) = 0;                      /* Internal = STATUS_SUCCESS */
                   MR32(ov+4) = (uint32_t)got;          /* InternalHigh = okunan */
                   uint32_t hev = MR32(ov+16);
                   if (hev){ void win32_signal_event(uint32_t); win32_signal_event(hev); }
                   g_overlapped_reads++;
               } else if (f->empty){
                   got = 0;                       /* aninda dosya sonu */
               } else {
                   got = read(f->fd, g_mem+buf, len);
                   if(got<0) got=0;
               }
               uint64_t _rd1 = now_ns_io();
               uint64_t _commit_ns = _commit0 && _rd0 >= _commit0 ? _rd0 - _commit0 : 0;
               uint64_t _service_ns = _rd0 && _rd1 >= _rd0 ? _rd1 - _rd0 : 0;
               atomic_fetch_add_explicit(&g_fileio_read_commit_ns, _commit_ns, memory_order_relaxed);
               atomic_fetch_add_explicit(&g_fileio_read_service_ns, _service_ns, memory_order_relaxed);
               uint64_t _maximum = atomic_load_explicit(&g_fileio_read_service_max_ns, memory_order_relaxed);
               while (_maximum < _service_ns && !atomic_compare_exchange_weak_explicit(
                       &g_fileio_read_service_max_ns, &_maximum, _service_ns,
                       memory_order_relaxed, memory_order_relaxed)) {}
               /* MUTEFILE=<parca>: eslesen dosyanin okumalarini SIFIRLA
                * doldur.  Muzik akisini (sdat.viv) susturup geriye ses
                * efekti kaliyor mu, bunu nesnel olcmek icin.  Dosya sonu
                * dondurmuyoruz ki akis mantigi normal calismaya devam
                * etsin -- yalnizca icerik sessizlesir. */
               if (fileio_should_mute_read(f->path, len, got))
                   memset(g_mem+buf, 0, (size_t)got);
               if(pnr) MR32(pnr)=(uint32_t)got;
               if (_readlog){
                   const char *bn = strrchr(f->path,'/'); bn = bn?bn+1:f->path;
                   double audio_time(void);
                   fprintf(stderr,"  [okuma] t=%8.3f %-28s istenen=%-9u alinan=%-9ld sure=%.2f ms\n",
                           audio_time(), bn, len, (long)got,
                           (double)_service_ns/1e6);
               }
               g_file_read++; g_bytes_read += (uint64_t)got;
               atomic_fetch_add_explicit(&g_fileio_snapshot_read_calls, 1u,
                                         memory_order_relaxed);
               atomic_fetch_add_explicit(&g_fileio_snapshot_read_bytes,
                                         (uint64_t)got, memory_order_relaxed);
               r=1; }
    }
    else if (!strcmp(n,"GetOverlappedResult")){
        /* (hFile, lpOverlapped, lpNumberOfBytesTransferred, bWait) */
        uint32_t ov=A(c,1), pn=A(c,2);
        if (ov && pn && mem_is_committed(ov+7)) MR32(pn) = MR32(ov+4);
        r=1; }
    else if (!strcmp(n,"HasOverlappedIoCompleted")){ r=1; }
    else if (!strcmp(n,"WriteFile")){
        FH*f=fh_get(A(c,0)); uint32_t buf=A(c,1), len=A(c,2), pnw=A(c,3);
        if(!f||f->fd<0){
            uint32_t h=A(c,0);
            if(h>=0x10u && h<=0x13u && len){ mem_commit(buf,len);
                fprintf(stdout,"[guest] %.*s",(int)len,(char*)(g_mem+buf)); }
            if(pnw) MR32(pnw)=len; r=1; }
        else { ssize_t w=write(f->fd, g_mem+buf, len); if(w<0)w=0;
               if(pnw) MR32(pnw)=(uint32_t)w; r=1; }
    }
    else if (!strcmp(n,"SetFilePointer")){
        FH*f=fh_get(A(c,0)); int32_t dist=(int32_t)A(c,1); uint32_t method=A(c,3);
        if(!f||f->fd<0) r=0xFFFFFFFFu;
        else { int wh = method==1?SEEK_CUR : (method==2?SEEK_END:SEEK_SET);
               off_t p = lseek(f->fd, dist, wh);
               r = (p<0)?0xFFFFFFFFu:(uint32_t)p; }
    }
    else if (!strcmp(n,"GetFileSize")){
        FH*f=fh_get(A(c,0)); uint32_t ph=A(c,1);
        if(!f||f->fd<0) r=0xFFFFFFFFu;
        else { struct stat st; if(fstat(f->fd,&st)==0){ r=(uint32_t)st.st_size;
                 if(ph) MR32(ph)=(uint32_t)(((uint64_t)st.st_size)>>32); } else r=0xFFFFFFFFu; }
    }
    else if (!strcmp(n,"SetEndOfFile")){
        FH*f=fh_get(A(c,0));
        if(f&&f->fd>=0){ off_t p=lseek(f->fd,0,SEEK_CUR); r=(ftruncate(f->fd,p)==0); } else r=0;
    }
    else if (!strcmp(n,"FlushFileBuffers")){ FH*f=fh_get(A(c,0)); if(f&&f->fd>=0) fsync(f->fd); r=1; }
    else if (!strcmp(n,"CloseHandle")){
        FH*f=fh_get(A(c,0));
        if(!f) return 0;                       /* bizim degil - diger katman baksin */
        if(f->fd>=0) close(f->fd);
        if(f->dir) closedir(f->dir);
        f->used=0; r=1;
    }
    else if (!strcmp(n,"GetFileAttributesA")){
        char real[1024]; resolve(GS(A(c,0)), real, sizeof real);
        struct stat st;
        if(stat(real,&st)!=0) r=0xFFFFFFFFu;
        else r = S_ISDIR(st.st_mode) ? 0x10u : 0x80u;   /* DIRECTORY / NORMAL */
    }
    else if (!strcmp(n,"SetFileAttributesA")) r=1;
    else if (!strcmp(n,"DeleteFileA")){
        char real[1024]; resolve_w(GS(A(c,0)), real, sizeof real);
        fileio_case_cache_invalidate_path(real);
        r = (unlink(real)==0);
    }
    else if (!strcmp(n,"SHGetFolderPathA")){
        /* (hwnd, csidl, hToken, dwFlags, pszPath) -> S_OK
         * Sanal koku veriyoruz; resolve() bunu oyun dizinine esliyor.
         * Bos donerse oyun kayit/profil dizinini bulamiyor.            */
        uint32_t pp=A(c,4);
        if (pp){ mem_commit(pp, 260); snprintf((char*)(g_mem+pp), 260, "C:\\NFSU2"); }
        r=0; }
    else if (!strcmp(n,"CreateDirectoryA")){
        char real[1024]; resolve_w(GS(A(c,0)), real, sizeof real);
        mkparents(real);
        fileio_case_cache_invalidate_path(real);
        r = (mkdir(real,0755)==0 || errno==EEXIST);
    }
    else if (!strcmp(n,"MoveFileA")){
        char a[1024],b[1024]; resolve(GS(A(c,0)),a,sizeof a); resolve_w(GS(A(c,1)),b,sizeof b);
        mkparents(b);
        /* Kaynak oyun kokundeyse (salt okunur) tasima yerine kopyala. */
        if (g_wroot[0] && strncmp(a, g_wroot, strlen(g_wroot)) != 0) r = (uint32_t)copy_file(a, b);
        else {
            fileio_case_cache_invalidate_path(a);
            fileio_case_cache_invalidate_path(b);
            r = (rename(a,b)==0);
        }
    }
    else if (!strcmp(n,"CopyFileA")){
        char a[1024],b[1024]; resolve(GS(A(c,0)),a,sizeof a); resolve_w(GS(A(c,1)),b,sizeof b);
        r = (uint32_t)copy_file(a, b);
    }
    /* --- dizin tarama: WIN32_FIND_DATAA (320 bayt), +0 attr, +44 ad --- */
    else if (!strcmp(n,"FindFirstFileA")){
        const char *spec = GS(A(c,0));
        char win[1024]; snprintf(win,sizeof win,"%s",spec);
        char *bs = strrchr(win,'\\'); char *fs2 = strrchr(win,'/');
        char *sep = bs>fs2?bs:fs2;
        char pat[128]="*";
        if (sep){ snprintf(pat,sizeof pat,"%s",sep+1); *sep=0; } else { snprintf(pat,sizeof pat,"%s",win); win[0]=0; }
        /* Kayit katmani etkinse once yazma kokundeki, sonra oyun
         * kokundeki dizin taranir (ayni ad iki kez verilmez). */
        char real[1024], wreal[1024]; wreal[0] = 0;
        if (win[0]) resolve_in(g_root, win, real, sizeof real); else snprintf(real,sizeof real,"%s",g_root);
        if (g_wroot[0]){
            if (win[0]) resolve_in(g_wroot, win, wreal, sizeof wreal);
            else snprintf(wreal, sizeof wreal, "%s", g_wroot); }
        DIR *d1 = wreal[0] ? opendir(wreal) : 0;
        DIR *d2 = opendir(real);
        if(!d1 && !d2){ r=INVALID_HANDLE; }
        else { uint32_t h=fh_new(); FH*f=fh_get(h);
               if(!f){ if(d1) closedir(d1); if(d2) closedir(d2); r=INVALID_HANDLE; }
               else { f->dir=d1; f->dir2=d2; snprintf(f->pat,sizeof f->pat,"%s",pat);
                      snprintf(f->path1,sizeof f->path1,"%s",wreal);
                      snprintf(f->path,sizeof f->path,"%s",real);
                      char nm[260];
                      if (!find_next(f, nm, sizeof nm)){
                          if(f->dir) closedir(f->dir); if(f->dir2) closedir(f->dir2);
                          f->used=0; r=INVALID_HANDLE; }
                      else { uint32_t fd_=A(c,1);
                          if(fd_){ mem_commit(fd_,320); memset(g_mem+fd_,0,320);
                              MR32(fd_)=0x80; snprintf((char*)(g_mem+fd_+44),260,"%s",nm); }
                          r=h; } } }
    }
    else if (!strcmp(n,"FindNextFileA")){
        FH*f=fh_get(A(c,0)); r=0; char nm[260];
        if(f && (f->dir || f->dir2) && find_next(f, nm, sizeof nm)){
            uint32_t fd_=A(c,1);
            if(fd_){ mem_commit(fd_,320); memset(g_mem+fd_,0,320);
                MR32(fd_)=0x80; snprintf((char*)(g_mem+fd_+44),260,"%s",nm); }
            r=1; }
    }
    else if (!strcmp(n,"FindClose")){
        FH*f=fh_get(A(c,0));
        if(f){ if(f->dir) closedir(f->dir); if(f->dir2) closedir(f->dir2); f->used=0; } r=1;
    }
    else return 0;
    *out = r;
    return 1;
}

void fileio_report(void){
    if (g_overlapped_reads)
        fprintf(stderr,"  dosya: %llu asenkron (overlapped) okuma\n",
                (unsigned long long)g_overlapped_reads);
    fprintf(stderr,"  dosya: %llu acildi, %llu acilamadi, %llu okuma, %.1f MB\n",
            (unsigned long long)g_file_open,(unsigned long long)g_file_fail,
            (unsigned long long)g_file_read,(double)g_bytes_read/1048576.0);
}
