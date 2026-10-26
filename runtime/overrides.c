/* Lift edilmis fonksiyonlarin runtime sarmalayicilari.
 *
 * D3DX efekt parametre agaci CEVRIMLI ayristiriliyor (dugum 0 -> 6 -> 4..8 -> 6)
 * ve dugum dizisi kapasitesinin (8) bir fazlasi (indeks 8) isteniyor.
 * Kok neden hala acik (bkz. PROJECT.md). Burada iki koruma var:
 *   1) dizi sinirlari: kapasiteyi asan indekste guvenle don
 *   2) ziyaret kumesi: bir ust-seviye yuruyuste her dugum en fazla bir kez
 * Boylece yuruyus sonlanir ve oyun ilerleyebilir.                         */
#include "cpu.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "enhance.h"

void guest_backtrace(CPU*,int);

uint64_t g_cycle_break = 0, g_oob_node = 0, g_depth_guard = 0;
static uint64_t guard_total = 0, guard_cap = 0;
static CPU *g_guard_cpu = 0;

#define HEAP_LO 0x10000000u
#define HEAP_HI 0x2F000000u

/* Bir guest adresini iceren heap blogunu bul (baslik: +0 boyut, +4 imza) */
static int block_of(uint32_t addr, uint32_t *ubeg, uint32_t *uend){
    if (addr < HEAP_LO || addr >= HEAP_HI) return 0;
    for (uint32_t p = (addr & ~15u); p >= HEAP_LO; p -= 16u){
        if (!mem_is_committed(p+4)) return 0;
        if (MR32(p+4) == 0xA110C8EDu){
            uint32_t sz = MR32(p), beg = p + 16u, end = beg + sz;
            if (addr >= beg && addr < end){ *ubeg = beg; *uend = end; return 1; }
            return 0;
        }
        if (p < 16u) break;
    }
    return 0;
}

static void guard_hit(void){
    if (!guard_cap){ const char *e = getenv("GUARD_CAP");
                     guard_cap = e ? (uint64_t)atoll(e) : 2000000ull; }
    if (guard_total == 5000 && g_guard_cpu && getenv("GUARD_BT")){
        fprintf(stderr,"\n=== 5000. koruma isabetinde cagri zinciri ===\n");
        guest_backtrace(g_guard_cpu, 14);
    }
    if (++guard_total > guard_cap){
        fprintf(stderr,"\n*** KORUMA TAVANI (%llu) - efekt sistemi ilerleyemiyor.\n",
                (unsigned long long)guard_cap);
        { extern uint32_t g_watch_addr; extern void watch_dump(const char*,int);
          if (g_watch_addr) watch_dump("koruma tavani", 20); }
        { extern void (*g_report_fn)(void); if (g_report_fn) g_report_fn(); }
        _exit(6);
    }
}

/* --- ortak: dugum dizisi sinir + ziyaret kontrolu -------------------- */
typedef struct { uint64_t bits[8]; uint32_t tab; } VisSet;   /* 512 dugum */

/* Agac korumasi ARTIK VARSAYILAN OLARAK KAPALI.
 * Cevrimli ayristirma aslinda CPU emulasyonu hatalarindan kaynaklaniyordu
 * (x87 yuvarlamasi, SSE genisligi, adc/sbb tasimasi...).  Onlar duzeldikten
 * sonra yuruyus kendiliginden sonlaniyor; koruma yalnizca GUARD_ON=1 ile
 * acilir ve o zaman gecerli dugumleri de kesip agaci bozar.              */
static int g_guard_off = -1;
static int guard_off(void){
    if (g_guard_off < 0) g_guard_off = getenv("GUARD_ON") ? 0 : 1;
    return g_guard_off;
}

static int node_guard(CPU *c, uint32_t a1, int depth, VisSet *vs){
    if (guard_off()) return 0;
    uint32_t tab = c->ecx + 0x2cu;
    if (!mem_is_committed(tab)) return 0;
    uint32_t arr = MR32(tab), beg, end;
    if (block_of(arr, &beg, &end)){
        uint32_t cap = (end - arr) / 0x64u;
        if (a1 >= cap){ g_oob_node++; guard_hit(); return 1; }
    }
    if (depth == 0){                       /* ust-seviye: kumeyi sifirla */
        for (int i=0;i<8;i++) vs->bits[i]=0;
        vs->tab = arr;
    }
    if (arr == vs->tab && a1 < 512u){
        uint64_t *w = &vs->bits[a1 >> 6]; uint64_t b = 1ull << (a1 & 63);
        if (*w & b){ g_cycle_break++; guard_hit(); return 1; }
        *w |= b;
    }
    return 0;
}

/* --- sub_0066aa3f(a1, a2) : asil agac yuruyucusu, ret 8, HRESULT ----- */
void sub_0066aa3f_orig(CPU *c);
static _Thread_local int d_aa3f = 0;
static _Thread_local VisSet vs_aa3f;

/* Efekt parametre agacinin dugum dizisini dok (tani).
 * Dugum: 0x64 bayt.  +0x04 dizi tabani, +0x18/+0x1a alt dugum indeksleri
 * (0xFFFF = yok), +0x58 tur etiketi.                                     */
static void dump_nodes(CPU *c){
    uint32_t tab = c->ecx + 0x2cu;
    if (!mem_is_committed(tab)) return;
    uint32_t arr = MR32(tab), beg, end;
    if (!block_of(arr, &beg, &end)){ fprintf(stderr,"  [agac] dizi 0x%08x blok disi\n", arr); return; }
    uint32_t cap = (end - arr) / 0x64u;
    fprintf(stderr,"  [agac] dizi=0x%08x kapasite=%u (blok %08x..%08x)\n", arr, cap, beg, end);
    /* ham dokum: dugum yapisini anlamak icin */
    for (uint32_t i = 0; i < cap && i < 4; i++){
        uint32_t n = arr + i*0x64u;
        if (!mem_is_committed(n+0x63)) break;
        fprintf(stderr,"    dugum[%u] ham @%08x:\n", i, n);
        for (int rw = 0; rw < 0x64; rw += 16){
            fprintf(stderr,"      +%02x:", rw);
            for (int q = 0; q < 16 && rw+q < 0x64; q++)
                fprintf(stderr," %02x", MR8(n+rw+q));
            fprintf(stderr,"   ");
            for (int q = 0; q < 16 && rw+q < 0x64; q++){
                uint8_t ch = MR8(n+rw+q);
                fputc((ch>=32&&ch<127)?ch:'.', stderr);
            }
            fprintf(stderr,"\n");
        }
        /* +0 ve +4 nereyi gosteriyor? */
        for (int f = 0; f <= 4; f += 4){
            uint32_t ptr = MR32(n+f);
            if (ptr && mem_is_committed(ptr+31)){
                fprintf(stderr,"      [+%d]->%08x:", f, ptr);
                for (int q=0;q<24;q++) fprintf(stderr," %02x", MR8(ptr+q));
                fprintf(stderr,"   ");
                for (int q=0;q<24;q++){ uint8_t ch=MR8(ptr+q);
                    fputc((ch>=32&&ch<127)?ch:'.', stderr); }
                fprintf(stderr,"\n");
            }
        }
    }
    for (uint32_t i = 0; i < cap && i < 64; i++){
        uint32_t n = arr + i*0x64u;
        if (!mem_is_committed(n+0x5C)) break;
        uint32_t nameptr = MR32(n);
        char nm[40]; nm[0]=0;
        if (nameptr && mem_is_committed(nameptr)){
            for (int k=0;k<32;k++){ uint8_t ch=MR8(nameptr+k);
                if (!ch || ch<32 || ch>126){ nm[k]=0; break; } nm[k]=(char)ch; nm[k+1]=0; }
        }
        fprintf(stderr,"    [%2u] ad=0x%08x '%s' taban=0x%08x alt18=%5u alt1a=%5u tur=0x%08x\n",
                i, nameptr, nm, MR32(n+4),
                MR16(n+0x18), MR16(n+0x1a), MR32(n+0x58));
    }
}

/* Ozyineleme zinciri: (dugum indeksi, dizi tabani) ciftleri.
 * Cevrimi somut gormek icin derinlik esigi asilinca dokulur. */
static _Thread_local uint32_t chain_idx[160], chain_arr[160];
static int chain_dumped = 0;

void sub_0066aa3f(CPU *c){
    uint32_t a1 = MR32(c->esp + 4u);
    g_guard_cpu = c;
    { static int dumped = 0;
      if (!dumped && getenv("TREEDUMP")){ dumped = 1; dump_nodes(c); } }
    if (getenv("WALKLOG")){
        static int nlog = 0;
        if (nlog < 60){ nlog++;
            uint32_t tabp = c->ecx + 0x2cu;
            uint32_t arr = mem_is_committed(tabp) ? MR32(tabp) : 0;
            uint32_t esi = arr + a1*0x64u;
            if (mem_is_committed(esi+0x35))
                fprintf(stderr,"    [yuru] derinlik=%2d a1=%-5u dugum=%08x "
                        "alt18=%5u alt1a=%5u sayi=%u taban=%08x\n",
                        d_aa3f, a1, esi, MR16(esi+0x18), MR16(esi+0x1a),
                        MR16(esi+0x34), MR32(esi+4));
            else
                fprintf(stderr,"    [yuru] derinlik=%2d a1=%-5u dugum=%08x ERISILEMEZ\n",
                        d_aa3f, a1, esi);
        }
    }
    if (d_aa3f < 160){ chain_idx[d_aa3f] = a1;
        chain_arr[d_aa3f] = mem_is_committed(c->ecx+0x2c) ? MR32(c->ecx+0x2c) : 0; }
    if (!chain_dumped && d_aa3f >= 40 && getenv("CHAIN")){
        chain_dumped = 1;
        fprintf(stderr,"  [zincir] ozyineleme derinligi %d, (indeks@dizi):\n", d_aa3f);
        for (int q = 0; q <= d_aa3f && q < 160; q++){
            fprintf(stderr," %u@%08x", chain_idx[q], chain_arr[q]);
            if ((q % 8) == 7) fprintf(stderr,"\n");
        }
        fprintf(stderr,"\n");
    }
    if (node_guard(c, a1, d_aa3f, &vs_aa3f)){
        POP32(c->eip); c->esp += 8u; c->eax = 0;   /* S_OK */
        return;
    }
    if (d_aa3f >= 128){ g_depth_guard++; guard_hit();
        POP32(c->eip); c->esp += 8u; c->eax = 0; return; }
    d_aa3f++;
    sub_0066aa3f_orig(c);
    d_aa3f--;
}

/* --- sub_0065ae35(a1, a2, a3) : yardimci yuruyucu, ret 0xc ---------- */
void sub_0065ae35_orig(CPU *c);
static _Thread_local int d_ae35 = 0;
static _Thread_local VisSet vs_ae35;

void sub_0065ae35(CPU *c){
    uint32_t a1 = MR32(c->esp + 4u);
    g_guard_cpu = c;
    if (node_guard(c, a1, d_ae35, &vs_ae35)){
        POP32(c->eip); c->esp += 12u; c->eax = 0;
        return;
    }
    if (d_ae35 >= 128){ g_depth_guard++; guard_hit();
        POP32(c->eip); c->esp += 12u; c->eax = 0; return; }
    d_ae35++;
    sub_0065ae35_orig(c);
    d_ae35--;
}

/* --- sub_006e2be0(list, b, c): listeyi gezip her ogeyi serbest birakir ---
 * Ilk arguman NULL gelebiliyor (Windows'ta olmayan bir durum). Orijinal kod
 * null kontrolu yapmadigi icin coker; guvenle donuyoruz.                 */
void sub_006e2be0(CPU *c);                     /* uretilen orijinal */
uint64_t g_null_list = 0, g_e2be0_calls = 0;
static void wrap_6e2be0(CPU *c){
    uint32_t a1 = MR32(c->esp + 4u);
    g_e2be0_calls++;
    if (a1 == 0 || !mem_is_committed(a1 + 4u)){
        g_null_list++;
        POP32(c->eip);                          /* ret (cdecl: cagiran temizler) */
        return;
    }
    sub_006e2be0(c);
}


/* --- sub_006623ea: efekt parametre degerini shader sabit blokuna kopyalar ---
 * __thiscall, 7 yigin argumani.  Yiginin duzeni (cagiran 0x00669710):
 *   [esp+04] a1  -> *(a1)=kaynak veri, *(a1+4)=tur tanimlayicisi
 *   [esp+08] a2  = hedef (sabit blogu icinde)
 *   [esp+0c] a3  = D3DXPARAMETER_TYPE (1=BOOL 2=INT 3=FLOAT)
 *   [esp+10] a4  = satir sayisi
 *   [esp+14] a5  = sutun sayisi
 * WVP matrisinin neden eksik yazildigini gormek icin girdi/cikti dokulur. */
void sub_006623ea_orig(CPU *c);
uint64_t g_copy_calls = 0;

void sub_006623ea(CPU *c){
    uint32_t a1 = MR32(c->esp + 4u),  dst = MR32(c->esp + 8u);
    uint32_t ty = MR32(c->esp + 12u), rows = MR32(c->esp + 16u);
    uint32_t cols = MR32(c->esp + 20u);
    uint32_t src = (a1 && mem_is_committed(a1+7)) ? MR32(a1) : 0;
    uint32_t td  = (a1 && mem_is_committed(a1+7)) ? MR32(a1+4) : 0;
    uint32_t cls = (td && mem_is_committed(td+7)) ? MR32(td+4) : 0xFFFFFFFFu;
    g_copy_calls++;
    /* FE efektinin sabit blogu 80 bayt; yalnizca ona yazan kopyalari izle */
    static int n = 0;
    int is80 = 0;
    { uint32_t b1, e1;
      if (block_of(dst, &b1, &e1) && (e1 - b1) == 80u) is80 = 1; }
    int show = 0;   /* kopya SONRASI karar verilecek */
    (void)show;
    /* MPROTSRC=1: FE efektinin WVP deger alanina KIM yaziyor?
     * Kopya kaynagi belli olunca sayfayi salt-okunur yapip yakalariz. */
    if (is80 && src && getenv("MPROTSRC")){
        static int armed3 = 0;
        uint32_t b2, e2;
        if (!armed3 && block_of(dst, &b2, &e2) && dst == b2){
            armed3 = 1;
            void mprot_arm(uint32_t);
            fprintf(stderr,"  [kopya] WVP kaynagi %08x -- sayfa korumaya alindi\n", src);
            mprot_arm(src);
        }
    }
    sub_006623ea_orig(c);
    /* Yalnizca 80 baytlik FE blokuna ANLAMLI bir deger yazan kopyalari dok */
    if (getenv("COPYLOG") && is80 && n < 14 && dst && mem_is_committed(dst+63)){
        int meaningful = 0;
        for (int q=0;q<16;q++){
            float fv = MRF32(dst + 4*q);
            if (fv != 0.f && fabsf(fv) > 1e-6f && fabsf(fv) < 1e6f) meaningful = 1;
        }
        if (meaningful){ n++;
            uint32_t bb, ee; block_of(dst, &bb, &ee);
            fprintf(stderr,"  [kopya %llu] a2=%u sinif=%d kaynak=%08x hedef=%08x"
                    " (blok %08x ofset %d)\n",
                    (unsigned long long)g_copy_calls, ty, (int)cls, src, dst,
                    bb, (int)(dst - bb));
            if (src && mem_is_committed(src+63)){
                fprintf(stderr,"      kaynak:");
                for (int r2=0;r2<4;r2++)
                    fprintf(stderr," [%.4g %.4g %.4g %.4g]", MRF32(src+r2*16),
                            MRF32(src+r2*16+4), MRF32(src+r2*16+8), MRF32(src+r2*16+12));
                fprintf(stderr,"\n");
            }
            fprintf(stderr,"      hedef :");
            for (int r2=0;r2<4;r2++)
                fprintf(stderr," [%.4g %.4g %.4g %.4g]", MRF32(dst+r2*16),
                        MRF32(dst+r2*16+4), MRF32(dst+r2*16+8), MRF32(dst+r2*16+12));
            fprintf(stderr,"\n");
            guest_backtrace(c, 8);
        }
    }
}

void overrides_install(void){
    void dispatch_patch(uint32_t, void(*)(CPU*));
    dispatch_patch(0x006e2be0u, wrap_6e2be0);
}

void overrides_report(void){
    if (g_e2be0_calls)
        fprintf(stderr,"  sub_006e2be0: %llu cagri, %llu NULL liste atlandi\n",
                (unsigned long long)g_e2be0_calls,(unsigned long long)g_null_list);
    if (g_cycle_break || g_oob_node || g_depth_guard)
        fprintf(stderr,"  agac korumasi: cevrim %llu, sinir disi %llu, derinlik %llu\n",
                (unsigned long long)g_cycle_break,
                (unsigned long long)g_oob_node,
                (unsigned long long)g_depth_guard);
}

/* --- sub_0050d510: oyunun HATA AYIKLAMA printf'i -----------------------
 * Perakende derlemede "xor eax,eax; ret" olarak bosaltilmis, yani motorun
 * tum ic gunlugu cope gidiyor: ön yüz paket gecisleri, mesaj kuyrugu,
 * "Joy Event" giris dagitimi...  Burada yakalayip yazdiriyoruz.
 * cdecl, degisken argumanli: bicim = [esp+4], argumanlar [esp+8]'den.  */
uint32_t g_fe_mgr = 0;   /* ön yüz paket yoneticisi (yakalanan) */
void sub_0050d510_orig(CPU *c);

static const char *gstr(uint32_t p){
    if (!p || !mem_is_committed(p)) return "(null)";
    return (const char *)(g_mem + p);
}
void sub_0050d510(CPU *c){
    static int on = -1;
    if (on < 0){ const char *e = getenv("GAMELOG"); on = (e && *e && *e != '0'); }
    if (on){
        uint32_t fp = MR32(c->esp + 4u);
        if (fp && mem_is_committed(fp)){
            const char *f = (const char *)(g_mem + fp);
            uint32_t ap = c->esp + 8u;          /* sonraki varargs slotu */
            char out[1024]; size_t o = 0;
            for (const char *q = f; *q && o < sizeof out - 8; q++){
                if (*q != '%'){ out[o++] = *q; continue; }
                q++;
                if (*q == '%'){ out[o++] = '%'; continue; }
                /* bayrak/genislik/kesinlik alanlarini atla */
                while (*q && (strchr("-+ #0", *q) || (*q >= '0' && *q <= '9') ||
                              *q == '.' || *q == 'l' || *q == 'h')) q++;
                char cv = *q;
                if (!cv) break;
                char tmp[256];
                if (cv == 's'){
                    uint32_t sp = mem_is_committed(ap+3) ? MR32(ap) : 0; ap += 4;
                    snprintf(tmp, sizeof tmp, "%s", gstr(sp));
                } else if (cv == 'c'){
                    uint32_t v = mem_is_committed(ap+3) ? MR32(ap) : 0; ap += 4;
                    snprintf(tmp, sizeof tmp, "%c", (int)(v & 0xFFu));
                } else if (cv == 'f' || cv == 'g' || cv == 'e'){
                    uint64_t lo = mem_is_committed(ap+7) ? ((uint64_t)MR32(ap) |
                                  ((uint64_t)MR32(ap+4) << 32)) : 0; ap += 8;
                    double dv; memcpy(&dv, &lo, 8);
                    snprintf(tmp, sizeof tmp, "%g", dv);
                } else {
                    uint32_t v = mem_is_committed(ap+3) ? MR32(ap) : 0; ap += 4;
                    if (cv == 'x') snprintf(tmp, sizeof tmp, "%x", v);
                    else if (cv == 'X') snprintf(tmp, sizeof tmp, "%X", v);
                    else if (cv == 'u') snprintf(tmp, sizeof tmp, "%u", v);
                    else if (cv == 'p') snprintf(tmp, sizeof tmp, "%08x", v);
                    else snprintf(tmp, sizeof tmp, "%d", (int)v);
                }
                for (const char *t2 = tmp; *t2 && o < sizeof out - 2; t2++) out[o++] = *t2;
            }
            out[o] = 0;
            fprintf(stderr, "  [oyun] %s%s", out,
                    (o && out[o-1] == '\n') ? "" : "\n");
            /* SwitchFEngPackages icinde ebx = ön yüz paket yoneticisi
             * ("this").  printf tam o noktada cagriliyor, yani isaretciyi
             * burada bedavaya yakaliyoruz; gecisi zorlamak icin gerekli. */
            if (!strncmp(out, "SwitchFEngPackages", 18)) g_fe_mgr = c->ebx;
            /* Ekran gecisini KIM tetikliyor?  Basarili bir gecisin cagri
             * zinciri, splash'te neyin eksik oldugunu gosterir. */
            { const char *bt = getenv("GAMELOG_BT");
              static int nbt = 0;
              if (bt && *bt && nbt < 4 && !strncmp(out, bt, strlen(bt))){
                  nbt++; guest_backtrace(c, 16); } }
        }
    }
    sub_0050d510_orig(c);
}


/* --- Yagmuru zorla (FORCERAIN) --------------------------------------------
 * Oyun havayi yaris basinda rastgele seciyor (olculdu: ~8 kosuda 1 yagmur);
 * yagmura ozgu bir sorunu ayiklamak icin tekrar uretilebilir yagmur gerek.
 * Rain::Update = sub_0061f130 (__thiscall, ecx = eView+0x64'teki Rain*):
 *   Rain+0x5094 hedef yagis (0..1), +0x1f0 o anki siddet (hedefe kayar),
 *   +0x509c bulut siddeti.  Hedef her karede sub_0061e390 ile hesaplanir
 *   ("Rain" bolge nesnesi 0x8a1f28 etkinse); etkin degilse eski hedef kalir.
 * FORCERAIN=1 hedefi 1.0 yapar, FORCERAIN=2 hedefi 0 yapar (hep kuru).
 * FORCERAIN=3 yagmursuz KAPALI gokyuzu (+0x509c ve hedefi +0x5098 = 0;
 * 1.0 = acik gokyuzu), FORCERAIN=4 kapali gokyuzu + yagmur.
 * Degisken verilmezse davranis AYNEN oyununki.  Iki build'de de var.      */
static int force_rain(void){
    static int fr = -1;
    if (fr < 0){ const char *e = getenv("FORCERAIN"); fr = e ? atoi(e) : 0; }
    return fr;
}
void sub_0061e390_orig(CPU *c);
void sub_0061e390(CPU *c){
    sub_0061e390_orig(c);
    int fr = force_rain();
    if (fr == 1 || fr == 4) ST(0) = 1.0; else if (fr == 2 || fr == 3) ST(0) = 0.0;
}
void sub_0061f130_orig(CPU *c);
void sub_0061f130(CPU *c){
    uint32_t rain = c->ecx;
    int fr = force_rain();
    if (fr && rain && mem_is_committed(rain + 0x50a3u))
        MRF32(rain + 0x5094u) = (fr == 1 || fr == 4) ? 1.0f : 0.0f;
    sub_0061f130_orig(c);
    if (fr >= 3 && rain && mem_is_committed(rain + 0x50a3u)){
        MRF32(rain + 0x5098u) = 0.0f; MRF32(rain + 0x509cu) = 0.0f; }
}

/* --- Gokyuzu gecisini zorla (FORCETOD) ------------------------------------
 * sub_0060fab0 = gunun-saati / gokyuzu durum makinesi (oyun durumu 6 iken
 * her kare).  Dunya sayaci 0x7fb718 (sn) uzerinden 300 sn'lik dongu:
 * (int)sayac % 300 == 60 ve durum 0x8a1c8c == 0 ise durum 2'ye gecer ve
 * gokyuzu gecisi (skymorph.efx: gun batimi -> gece) baslar.  Test kosulari
 * ~60 sn surdugu icin bu gecise ancak ara sira denk geliniyordu (olculdu:
 * ~8 kosuda 1) ve HD build o durumda cok yavasliyordu.
 * FORCETOD=1 gecisi ilk firsatta tetikler; FORCETOD=<kare sayisi> o kadar
 * cagridan sonra.  Verilmezse davranis AYNEN oyununki.  Iki build'de var. */
void sub_0060fab0_orig(CPU *c);
void sub_0060fab0(CPU *c){
    static int ft = -1, done = 0; static long calls = 0;
    if (ft < 0){ const char *e = getenv("FORCETOD"); ft = e ? atoi(e) : 0; }
    if (ft > 0 && !done && MR32(0x8654a4u) == 6u && MR32(0x8a1c78u) != 0u && ++calls >= ft &&
        MR32(0x8a1c8cu) == 0u){
        static int st = -1;
        if (st < 0){ const char *e = getenv("FORCETOD_STATE"); st = e ? atoi(e) : 2; if (st < 1 || st > 4) st = 2; }
        MR32(0x8a1c8cu) = (uint32_t)st; done = 1;
        fprintf(stderr, "[FORCETOD] gokyuzu durumu %d'ye zorlandi (cagri %ld)\n", st, calls);
    }
    sub_0060fab0_orig(c);
}

/* --- sub_005c7ed0: gorunum basina kamera/projeksiyon kurulumu ------------
 * __thiscall(ecx = matris yapisi, [esp+4] = eView*), ret 0x10.
 *   eView+0x04 = gorunum kimligi (1 ana, 3 dikiz aynasi, 4 yol yansimasi)
 *   eView+0x40 = Camera*  (CameraParams: +0xbc NearZ, +0xc0 FarZ, +0xc4 FOV)
 *   ecx+0x00 gorunum matrisi, +0x40 projeksiyon, +0x80 ikisinin carpimi
 * Oyun SetTransform'a yalnizca birim matris veriyor; kamera ancak buradan
 * okunabiliyor.  Davranis DEGISMEZ: yalnizca adres yakalanir ve (HD build'de)
 * ana gorunumun matrisleri kareye islenir.  Orijinal build'de enh_view_setup
 * bos bir satir-ici fonksiyon.                                             */
void sub_005c7ed0_orig(CPU *c);
void sub_005c7ed0(CPU *c){
    uint32_t plat = c->ecx, view = MR32(c->esp + 4u);
    sub_005c7ed0_orig(c);
    if (view && plat && mem_is_committed(view + 0x43u) && mem_is_committed(plat + 0xbfu))
        enh_view_setup(MR32(view + 4u), (const float *)(g_mem + plat),
                       MR32(view + 0x40u));
}


/* --- sub_00406460: ForceFeedback / Telemetri ve Fizik Titresim Kancasi ----
 * __thiscall(ecx = ForceFeedback* [Driver + 0x690])
 * Gercek oyun ici carpisma, duvar surtmesi, bordur/toprak ve drift titresimi. */
#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
extern void nfsu2_input_report_rumble(float continuous_intensity, float continuous_sharpness);
extern void nfsu2_input_trigger_impact(float intensity, float sharpness, uint32_t duration_ms);
extern void nfsu2_input_clear_rumble(void);
#else
static inline void nfsu2_input_report_rumble(float continuous_intensity, float continuous_sharpness){
    (void)continuous_intensity; (void)continuous_sharpness;
}
static inline void nfsu2_input_trigger_impact(float intensity, float sharpness, uint32_t duration_ms){
    (void)intensity; (void)sharpness; (void)duration_ms;
}
static inline void nfsu2_input_clear_rumble(void){ }
#endif
#else
static inline void nfsu2_input_report_rumble(float continuous_intensity, float continuous_sharpness){
    (void)continuous_intensity; (void)continuous_sharpness;
}
static inline void nfsu2_input_trigger_impact(float intensity, float sharpness, uint32_t duration_ms){
    (void)intensity; (void)sharpness; (void)duration_ms;
}
static inline void nfsu2_input_clear_rumble(void){ }
#endif

void sub_00406460_orig(CPU *c);

static uint32_t s_prev_veh = 0;
static int s_has_prev = 0;
static uint32_t s_prev_sim_tick = 0;
static float s_prev_vx = 0.0f;
static float s_prev_vy = 0.0f;
static float s_prev_vz = 0.0f;
static float s_prev_speed = 0.0f;
static uint32_t s_prev_drv_col = 0;
static int s_prev_wall_wheels = 0;
static int s_airborne_ticks = 0;

static void rumble_reset_state(void){
    s_prev_veh = 0;
    s_has_prev = 0;
    s_prev_sim_tick = 0;
    s_prev_vx = 0.0f;
    s_prev_vy = 0.0f;
    s_prev_vz = 0.0f;
    s_prev_speed = 0.0f;
    s_prev_drv_col = 0;
    s_prev_wall_wheels = 0;
    s_airborne_ticks = 0;
}

void sub_00406460(CPU *c){
    uint32_t ff = c->ecx;
    uint32_t veh = (ff && mem_is_committed(ff + 3u)) ? MR32(ff) : 0u;

    if (!veh || !mem_is_committed(veh + 0x433u) || MR32(veh + 0x28u) != 1u){
        /* AI veya gecersiz arac: yerel oyuncunun rumble durumuna DOKUNMA! */
        sub_00406460_orig(c);
        return;
    }

    /* Aktif Yaris ve Yerel Oyuncu Kontrolleri */
    uint32_t gfm = mem_is_committed(0x8900dbu) ? MR32(0x8900d8u) : 0u;
    int racing_active = (gfm && mem_is_committed(gfm + 3u) && MR32(gfm) == 4u) ||
                        (mem_is_committed(0x8654a7u) && MR32(0x8654a4u) == 6u);
    int not_paused = !mem_is_committed(0x89e7d8u) || (MR8(0x89e7a0u + 0x38u) == 0u);
    int not_replay = !mem_is_committed(0x89010bu) || (MR32(0x890108u) == 0u);

    if (MR32(veh + 0x18u) == 0u || !racing_active || !not_paused || !not_replay){
        nfsu2_input_clear_rumble();
        rumble_reset_state();
        sub_00406460_orig(c);
        return;
    }

    /* Ayni Fizik Karede Cift Cagri Korumasi (Deduplication) */
    uint32_t sim_tick = mem_is_committed(0x86518bu) ? MR32(0x865188u) : 0u;
    float vx = MRF32(veh + 0x70u), vy = MRF32(veh + 0x74u), vz = MRF32(veh + 0x78u);
    if (s_has_prev && s_prev_veh == veh && sim_tick != 0u && sim_tick == s_prev_sim_tick &&
        vx == s_prev_vx && vy == s_prev_vy && vz == s_prev_vz){
        sub_00406460_orig(c);
        return;
    }

    float speed = sqrtf(vx * vx + vy * vy + vz * vz);
    if (isnan(speed) || isinf(speed) || speed > 200.0f) speed = 0.0f;
    if (speed < 0.001f){
        float alt_spd = fabsf(MRF32(veh + 0x430u));
        if (!isnan(alt_spd) && !isinf(alt_spd) && alt_spd < 200.0f){
            speed = alt_spd;
        }
    }

    if (s_prev_veh != veh) s_has_prev = 0;

    float vec_dv = 0.0f;
    float speed_drop = 0.0f;
    if (s_has_prev){
        float dvx = vx - s_prev_vx, dvy = vy - s_prev_vy, dvz = vz - s_prev_vz;
        vec_dv = sqrtf(dvx * dvx + dvy * dvy + dvz * dvz);
        if (isnan(vec_dv) || isinf(vec_dv)) vec_dv = 0.0f;
        speed_drop = (s_prev_speed > speed) ? (s_prev_speed - speed) : 0.0f;
    }

    /* Rigid-Body Carpisma Zaman Damgasi ve Surucu Carpisma Durumu */
    uint32_t col_body = mem_is_committed(veh + 0x6fbu) ? MR32(veh + 0x6f8u) : 0u;
    int body_hit = (col_body && mem_is_committed(col_body + 0x1e3u) && sim_tick != 0u && MR32(col_body + 0x1e0u) == sim_tick);
    uint32_t driver = (ff >= 0x690u) ? (ff - 0x690u) : 0u;
    uint32_t drv_col = (driver && mem_is_committed(driver + 0x253u)) ? MR32(driver + 0x250u) : 0u;

    /* Tekerlek Zemin Temasi (veh + 0x390..0x393) ve Yuzey Materyali (veh + 0x394..0x3a0) */
    int wheels_on_ground = 0;
    int wall_wheels = 0;
    int rough_wheels = 0;
    for (uint32_t i = 0; i < 4u; i++){
        uint8_t on_gnd = MR8(veh + 0x390u + i);
        if (on_gnd) wheels_on_ground++;
        uint32_t surf = MR32(veh + 0x394u + i * 4u);
        if (surf == 0xdu) wall_wheels++;
        else if (on_gnd && (surf - 8u) <= 9u) rough_wheels++;
    }

    /* 4 Tekerlek Kayma / Drift (veh + 0x370..0x37c) */
    float slip_sum = fabsf(MRF32(veh + 0x370u)) + fabsf(MRF32(veh + 0x374u)) +
                     fabsf(MRF32(veh + 0x378u)) + fabsf(MRF32(veh + 0x37cu));
    if (isnan(slip_sum) || isinf(slip_sum)) slip_sum = 0.0f;

    /* Darbe (Impact) Tetikleme Kosullari */
    if (s_has_prev && (vec_dv >= 0.70f || speed_drop >= 0.60f ||
        (body_hit && (vec_dv >= 0.25f || speed >= 1.5f)) ||
        (drv_col >= 1u && drv_col <= 4u && s_prev_drv_col == 0u) ||
        (wall_wheels > 0 && s_prev_wall_wheels == 0 && speed >= 1.2f))){
        float imp = (vec_dv - 0.45f) / 5.5f;
        float drop_imp = (speed_drop - 0.35f) / 4.5f;
        if (drop_imp > imp) imp = drop_imp;
        if (body_hit && imp < 0.45f) imp = 0.45f + fminf(speed / 35.0f, 0.45f);
        if (wall_wheels > 0 && s_prev_wall_wheels == 0 && imp < 0.50f) imp = 0.50f + fminf(speed / 30.0f, 0.45f);
        if (drv_col >= 1u && drv_col <= 4u && s_prev_drv_col == 0u && imp < 0.55f) imp = 0.55f;
        if (imp < 0.35f) imp = 0.35f;
        if (imp > 1.0f) imp = 1.0f;
        float sharp = 0.45f + 0.55f * imp;
        uint32_t dur = (uint32_t)(90.0f + 160.0f * ((imp - 0.35f) / 0.65f));
        if (dur < 90u) dur = 90u;
        if (dur > 250u) dur = 250u;
        nfsu2_input_trigger_impact(imp, sharp, dur);
    }

    /* Ziplama Sonrasi Yere Inis Darbesi (Jump Landing Thud) */
    if (wheels_on_ground == 0){
        s_airborne_ticks++;
    } else {
        if (s_airborne_ticks >= 6 && s_has_prev){
            float land_imp = fminf(0.35f + (float)s_airborne_ticks * 0.035f + fabsf(vz - s_prev_vz) * 0.12f, 0.90f);
            nfsu2_input_trigger_impact(land_imp, 0.40f, 120u);
        }
        s_airborne_ticks = 0;
    }

    /* Surekli Titresim (nfsu2_input_report_rumble) */
    float cont_intensity = 0.0f;
    float cont_sharpness = 0.0f;
    if (wall_wheels > 0 && speed > 1.0f){
        cont_intensity = fminf(0.40f + speed * 0.025f, 0.90f);
        cont_sharpness = 0.75f;
    }
    if (rough_wheels > 0 && speed > 2.0f){
        float r_int = fminf(0.18f + (float)rough_wheels * 0.06f + speed * 0.012f, 0.58f);
        if (r_int > cont_intensity){
            cont_intensity = r_int;
            cont_sharpness = 0.40f;
        }
    }
    if (slip_sum > 0.12f && speed > 1.5f && wheels_on_ground > 0){
        float s_int = fminf((slip_sum - 0.12f) * 0.22f, 0.48f);
        if (s_int > cont_intensity){
            cont_intensity = s_int;
            cont_sharpness = 0.25f;
        }
    }
    nfsu2_input_report_rumble(cont_intensity, cont_sharpness);

    s_prev_veh = veh;
    s_has_prev = 1;
    s_prev_sim_tick = sim_tick;
    s_prev_vx = vx;
    s_prev_vy = vy;
    s_prev_vz = vz;
    s_prev_speed = speed;
    s_prev_drv_col = drv_col;
    s_prev_wall_wheels = wall_wheels;

    sub_00406460_orig(c);
}
