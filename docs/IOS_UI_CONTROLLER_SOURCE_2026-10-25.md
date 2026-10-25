# iOS arayüz ve kontrolcü kaynak uygulaması — 25 Ekim 2026

Taban: `cdcf31f0a6bc229aaac8f72b14925be4f4c93369` (v0.26.1/build34).
Bu çalışma kaynak değişikliğidir. Yeni sürüm, IPA, CI koşusu veya push yapılmadı.
Var olan `FullRuntimeSmokeTests.swift` taşınan durum metnine Ayarlar üzerinden
erişecek şekilde uyarlandı; `tests/audio_core_test.c` içine 48 kHz örnek çerçevesi
hizalaması ve yükleme ekranı akış duraksaması birim testi eklendi.

## Oyun yüzeyi ve ayarlar

- Başarılı runtime durum metni, log paylaşımı ve kontrolcü adı/pili oyun
  yüzeyinden kaldırıldı; dişli düğmesinin açtığı Türkçe Ayarlar paneline taşındı.
  Gerçek runtime hataları oyun üzerinde görünür kalır. Dosya import/başlatma
  akışı korunur.
- Dokunmatik Onay / Atla (Enter), Geri (Backspace) ve Esc düğmeleri kol bağlı
  olduğunda da erişilebilir. Başlatma ekranı kendi kendine atlanmaz; tuşlar
  kullanıcının basışıyla üretilir. `NOMOVIES` değiştirilmedi.
- Dokunmatik görünürlüğü (otomatik/göster/gizle), kaydırıcı/D-pad tercihi ve
  manuel vites görünürlüğü `UserDefaults` içinde saklanır. Kolun yeniden
  bağlanması açık görünürlük tercihini sıfırlamaz.

## Kontrolcü davranışı

`ControllerSettings.swift` kontrolcü yönetimini ve kalıcı ayarları toplar.
Direksiyon ölü bölgesi, tetik basma eşiği, ters direksiyon, tetik değişimi ve
aksiyon tuşu eşlemeleri gerçek DIK üretimine bağlıdır. Mevcut C köprüsü analog
değerleri dijital tuşlara dönüştürdüğünden bu ayarlar sürekli analog
direksiyon/gaz hassasiyeti olarak sunulmaz.

Temel eşlemeler korunur: R2 gaz, L2 fren, sol çubuk yön, D-pad oklar/WASD;
Çarpı/A Enter, Daire/B ve L1 el freni, Kare/X ve R1 nitro, Üçgen/Y kamera,
L3 sıfırla, R3 arkaya bak, Options/Menu Esc. Önceden Esc ile aynı işlevdeki
Share/View varsayılanı Geri (Backspace) yapıldı ve yeniden eşlenebilir.
Mikro gamepad için A onay, X geri, Menu Esc kullanılır.

Kontrolcü girdileri ana kuyrukta işlenir. Aynı DIK'e eşlenen birden fazla
kol düğmesi birleşik durumda tutulur. Dokunmatik ve kontrolcü ayrı kaynaklar
olarak C köprüsünde OR ile birleştirilir. Ayarlar/arka plan geçişinde girdiler
temizlenir, input gate kapatılır; devam ederken tuş bazlı bastırma (`suppressedKeys`)
sayesinde yalnız menü kapanışı veya yeniden bağlanma anında basılı tutulan tuşlar
bırakılana kadar bastırılır, analog çubukta hafif sapma olsa bile diğer tüm tuşlar
(ör. Çarpı/Enter) ilk basışta anında yanıt verir. Dokunmatik düğmeler, pedallar ve
Metal kanvas tıklamaları (`pressToken` / `pointerTouchToken`) 50 ms minimum basılı
tutma garantisiyle korunur; hızlı çift dokunuşlarda eski zamanlayıcı yeni basışı
erken bırakmaz. Eski kontrolcünün handler'ı çıkarılır. Pil bilgisi ayarlar açılışında
ve bağlı kol için periyodik yenilenir; bilinmeyen pil yüzdesi uydurulmaz.

## Rachel akışı, FEng pencere mesajları ve DirectInput düzeltmeleri

EXE'nin import ve mesaj pompası incelemesi hem `DirectInput8Create` hem de
standart Win32 pencere mesaj pompasını (`PeekMessageA` / `GetMessageA` /
`DispatchMessageA`) gösterir. Bu çalışma hem DirectInput veri akışındaki
kusurları hem de FEng ön yüzünün beklediği Win32 pencere mesajlarını ve
klavye/fare durumu sorgularını birlikte tamamlar:

- `win32_ext.c` içinde `WMQ` kuyruğu `8`'den `64` girdiye çıkarıldı ve
  `PeekMessageA` / `GetMessageA` başında çalışan `wm_poll_input()` eklendi:
  - İmleç hareket ettiğinde `WM_MOUSEMOVE` (`0x0200`, `wParam = ldown ? MK_LBUTTON : 0`,
    `lParam = (my << 16) | (mx & 0xFFFF)`),
  - Sol tıklama/dokunma başladığında ve bittiğinde `WM_LBUTTONDOWN` (`0x0201`) ve
    `WM_LBUTTONUP` (`0x0202`),
  - Temel navigasyon tuşlarında (`DIK_RETURN`/`VK_RETURN`, `DIK_ESCAPE`/`VK_ESCAPE`,
    `DIK_BACK`/`VK_BACK`, `DIK_SPACE`/`VK_SPACE`, yön tuşları) basış/bırakış kenarı
    oluştuğunda `WM_KEYDOWN` (`0x0100`) ve `WM_KEYUP` (`0x0101`) mesajları
    üretilerek `DispatchMessageA` üzerinden oyunun `WndProc` yordamına iletilir;
    ayrıca `MSG.pt` (`pm+20`, `pm+24`) güncel imleç koordinatlarıyla doldurulur.
- `win32_ext.c` içindeki `GetKeyboardState`, `GetAsyncKeyState` ve `GetKeyState`
  stub'ları `metal_fill_keyboard` ve `metal_fill_mouse` üzerinden canlı tuş/fare
  durumu döndürecek şekilde bağlandı.
- `DIGDD_PEEK` isteklerinde buffer gerçekten doldurulur; olay tüketilmez ve
  `DI_BUFFEROVERFLOW` durumu peek sırasında da korunur.
- `rgdod == NULL` ve `want < 0xFFFFFFFF` ile yapılan olay sayısı sorguları
  (`consume = (rg != 0 || want == 0xFFFFFFFFu) && !peek`) kuyruktaki olayları
  yanlışlıkla tüketip silmez.
- `SPEED2.EXE`'nin FrontEnd / Menü eylem haritalarında (`0x007fd7e0`, `0x007fd330`,
  `0x007fd650`) geri/iptal eylemi (`uAppData = 0x22`) yalnız `DIK_ESCAPE` (`0x01`)
  tuşuna bağlı olduğu ve `DIK_BACK` (`0x0E`, Backspace) tabloda yer almadığı için,
  `SetActionMap` içinde `uAppData == 0x22` olduğunda `DIK_BACK` (`0x0E`) de aynı
  eylem ve ofsete bağlandı; böylece menülerde/bilgilendirmelerde dokunmatik `GERİ`
  ve DS4 `Share/View` tuşu gerçek geri eylemini tetiklerken yarış içi `Action Map 0`
  (`uAppData = 0x0C` Pause) etkilenmez.
- Geçerli `uAppData=0` eşlemesi, eşlenmemiş tuşla karıştırılmaz.
- Action map kurulmadan raw keyboard buffered olayları da döner.
- iOS'ta iki snapshot arasındaki kısa basış/bırakışlar 512 kayıtlık sınırlı
  olay kuyruğunda korunur. Diğer platformlar mevcut snapshot yolunu kullanır.
- Klavye action-map state'i, tanımlı DWORD ofsetlerinden doldurulur;
  16/20 baytlık klavye biçimi yanlışlıkla fare state'i sayılmaz. Fare cihazı
  klavye action map'ini veya klavye olay kuyruğunu tüketmez.
- Açık `SetDataFormat` çağrısı action-map modundan raw moda dönüşü ele alır.
- Null buffer ile sayma/tüketme/flush, peek ve overflow davranışları ayrıdır.
  Temizleme nesli değişince daha önce iletilmiş basışların bırakılması yeni
  olaylardan önce uzlaştırılır. Eski basışlar tekrar oynatılmaz.

Olay slotları atomiktir; kuyruk cursor/nesil kontrolleri temizleme sırasında
bayat kaydın tüketilmesini engeller. Üretim input timestamp'i mevcut
runtime'ın `host_us()/1000` saatini kullanır; QPC/TSC/scheduler değişmez.
Yalnız `GetDeviceState` kullanan akış 256 baytlık birleşik state'i okumaya
devam eder. Bu kaynak iyileştirmeleri Rachel ekranında cihaz denemesi
yapılmış olduğu anlamına gelmez; o ekranın gerçek tüketici çağrı izi bu
çalışmada runtime üzerinde alınmadı.

API davranışı için resmi kaynaklar:

- [Microsoft GetDeviceData: peek, null buffer, flush ve overflow](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee417894(v=vs.85))
- [Microsoft DIDEVICEOBJECTDATA: ofset, zaman ve uAppData](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee416611(v=vs.85))

## Gerçek kontrolcü titreşimi ve sınırı

`GCController.haptics` → `supportedLocalities` →
`createEngine(withLocality: .default)` → `CHHapticPatternPlayer` yolu
kontrolcünün aktüatörlerine sonlu, kısa desen gönderir. Bu yol mevcut
`UIImpactFeedbackGenerator` cihaz dokunma hissinden ayrıdır. Motor ve player
işleri ayrı seri kuyrukta yapılır; render döngüsünde motor oluşturulmaz.

Titreşim aç/kapat, şiddet, kullanıcı tetiklemeli kısa deneme ve isteğe bağlı
aksiyon tuşu geri bildirimi saklanır. Destek veya motor oluşturma yoksa deneme
düğmesi devre dışıdır ve durum açıklanır. Menü/lifecycle/controller değişimi
player ve motoru durdurur; reset sonrası eski player tekrar kullanılmaz.

**Oyun force feedback entegrasyonu tamamlanmış değildir.** Runtime'ın
DirectInput tablosunda effect metot adları vardır; gerçek `CreateEffect`,
effect parametreleri ve Start/Stop üretimi bu kaynakta bağlı değildir.
Çarpışma, motor ve yol sinyalleri uydurulmaz. Eklenen otomatik titreşim
kullanıcı aksiyonunun geri bildirimidir; kısa deneme yalnız kullanıcı isteğidir.

Doğrulanan resmi API kaynakları:

- [GCDeviceHaptics: desteklenen localities ve kontrolcü motoru](https://developer.apple.com/documentation/gamecontroller/gcdevicehaptics)
- [createEngine(withLocality:)](https://developer.apple.com/documentation/gamecontroller/gcdevicehaptics/createengine(withlocality:))
- [Engine stoppedHandler ve callback kuyrukları](https://developer.apple.com/documentation/corehaptics/chhapticengine/stoppedhandler-swift.property)
- [Engine durma nedenleri: disconnect, suspend, interruption](https://developer.apple.com/documentation/corehaptics/chhapticengine/stoppedreason)
- [GameController input callback kuyruğu](https://developer.apple.com/documentation/gamecontroller/gcdevice/handlerqueue)

## Grafik varsayılanları ve güvenli performans optimizasyonları

İlk kez sorgulanan ve kayıtlı değeri olmayan iOS grafik DWORD'leri gerçek
registry yolunda saklanır. `NotFirstTime=1` döndürüldüğünde oyun `0x005be880`
donanım önayar kurulumunu atlayıp doğrudan `0x005bea30` fonksiyonundaki 27
anahtarlık `RegQueryValueExA` yükleyicisine geçtiği için, `tools/nfsref/nfsref_kayit.txt`
referans profilindeki 27 grafik anahtarının tamamı (`g_RacingResolution=3`,
`g_WorldLodLevel=3`, `g_CarLodLevel=1`, `g_PerformanceLevel=5`, yol/araç
yansımaları, gölge, far, motion blur, light trails/glow, parçacık, overbright,
bleach bypass, tinting, yağmur, ufuk sisi, anizotropik filtreleme `2`,
`g_FSAALevel=0`, `g_VSyncOn=0`) varsayılan olarak tanımlandı. Kayıtlı değerler
önce okunur ve korunur. Kayıt/import dosyası silinmez, zorlayıcı migration yoktur.
Ayrıca iOS üzerinde `scr_cache()`, `metal_native_size()` ve `win_apply_size()`
içinde `UIScreen.scale` yerine `nativeScale` ve yatay ekran sınırları kullanılarak
`ROW_NATIVE=3` çözünürlüğünün tam piksel bütçesiyle eşleşmesi sağlandı.

Ek olarak çalışan v0.26.1 zamanlayıcı/saat/GPUVS davranışını değiştirmeden
aşağıdaki düşük riskli sıcak yol optimizasyonları uygulandı:

- `metal_render_frame` içinde her kare için `@autoreleasepool` eklendi;
  `alfavar_mode()`, `abt_off("ALFAVAR")` ve `hd_msaa_l2()` çizim döngüsü dışına
  taşındı; `samp_for()` için 1-girişli MRU önbellek eklendi; `c_fp` piksel sabit
  kontrolünde 560 baytlık yığın kopyasını atlayan hızlı yol ve açılmış 4 aşamalı
  doku bağlama (`ts[0..3]`) devreye alındı.
- `win32.c` ve `win32_ext.c` içinde `GetLastError`/`SetLastError`, `cpu_tid_checked`
  ve `guest_note_import` için `g_memory_mutex` kilitleyen `guest_memory_pointer`
  yerine kilitsiz `mem_is_committed` + `g_mem` erişimi kullanıldı; `cs_for()` için
  thread-local MRU önbellek ve `win32_call` içinde `EnterCriticalSection`,
  `LeaveCriticalSection`, `TlsGetValue`, `GetLastError`, `SetLastError` için indeks
  önbelleği eklendi.
- `fileio.c` içinde `CreateFileA` üzerindeki mükerrer `stat()` çağrısı kaldırıldı,
  5 dilli `_en/_dn/_du/_sw` yedek araması yalnız `_[a-z]{2}` sonekli dosyalara
  sınırlandı, `find_next` içinde yazma kökü dizini yokken yapılan gereksiz `stat()`
  kontrolü önlendi ve Darwin salt okunur dosya açılışlarında `F_RDAHEAD` etkinleştirildi.
- `com.c` DirectSound `ds_lock_match` içinde sarma (wrap-around) durumunda `n2 == 0`
  iken `p2 == NULL` ile yapılan geçerli `Unlock` çağrılarının reddedilmesi düzeltildi
  ve yedek imleç hesabı 4 baytlık örnek çerçevesine (`& ~3u`) hizalandı.
- `audio.m` içinde 48 kHz donanım örnekleme hızı hizalaması ve yükleme ekranı
  akış koruması tamamlandı:
  - `FRAMES` dilim sınırı `2048`'den `4096` kareye çıkarıldı ve rota değişiminde
    (`audio_ios_reconfigure_sample_rate`) `kAudioUnitProperty_MaximumFramesPerSlice`
    yeniden uygulandı.
  - `audio_cursors_locked` içinde `play` ve `write` imleçleri, 44.1 kHz ↔ 48 kHz
    kesirli `b->cursor` ilerlemelerinde bile tam örnek çerçevesine (`bpf`)
    hizalandı (`((raw_p / bpf) * bpf) % sz`).
  - Oynatma sırasında parçalı akış yazması alan döngüsel tamponlar (`b->stream_writes >= 2`)
    izlenerek, yükleme ekranlarında konuk üretici iş parçacığı tampon süresinden
    (`buf_us`, en az 250 ms) daha uzun süre yeni ses yazmadığında (`stalled_stream`),
    imleç ilerletilirken bayat PCM örneklerinin tekrar tekrar karıştırılması
    susturuldu; statik döngüsel sesler (`stream_writes < 2`) korundu.
  - `mix_chunk` içinde 16-bit stereo ve 16-bit mono tamponlar için örnek başına
    `memcpy` ve kanal dallanmasını kaldıran doğrudan indeksli hızlı döngüler eklendi.
- `d3dx_hle.c`, `vsemu.c`, `enhance.c`, `res.c`, `mem.c`, `metal.m`, `com.c` ve
  `NFSU2InputBridge.c` içinde ek sıcak yol optimizasyonları uygulandı (`mem_commit`
  hızlı taahhüt kontrolü, `dss_for` ve `pso_for` 1-entry MRU önbellekleri, `vs_compile`
  sırasında sabit sorgu önbelleklemesi ve `vs_run` hızlı matris/hedef yazımı, `res_find`
  sayısal kaynak önbelleği, `d3dx_hle` başlatma zamanı `getenv` önbelleklemesi ve
  `n5_pass`/`n4_core`/`sw_put` döngü optimizasyonları).

Salt okunur EXE incelemesi bu isimlerin gerçek oyun tarafından registry'den
okunduğunu doğruladı (`0x5bea30` çevresindeki sorgular):

| Anahtar | Oyun veri adresi | Varsayılan |
| --- | --- | --- |
| `g_WorldLodLevel` | `0x870cf4` | 3 |
| `g_CarLodLevel` | `0x870cf8` | 1 |
| `g_RacingResolution` | `0x870d1c` | 3 |
| `g_PerformanceLevel` | `0x87099c` | 5 |

`0x5d2909` world LOD için 0..3 dal tablosunu, `0x5d28e0` car LOD için
0/1 yollarını kullanır. Seçilen LOD değerleri tanınan aralığın üst ucudur
ve referans profiliyle aynıdır. Geometri yoğunluğunun yönü ve cihazdaki
görsel sonuç bu statik aralık kontrolüyle tek başına ölçülmüş sayılmaz.

`runtime/com.c` içindeki mevcut altı mod ve `ROW_NATIVE=3` değişmez. Satır 3
viewport piksel boyutundan hesaplanır ve `res_pick` ile gerçek render
boyutuna gider. Swift yüzeyi bağlı ekranın `nativeScale` değeriyle layout
boyutunu piksele çevirir; çözünürlük hardcode edilmez. Mevcut piksel üst
sınırı korunur. Sampler mip/LOD ayarları bu iş için değiştirilmedi.
Viewport henüz yokken kullanılan tarihsel `2560x1600` fallback değiştirilmedi;
normal iOS başlangıcı pozitif view boyutu ve hazır Metal katmanı bekler.

Gerçek full-runtime kaynak tarifi `tools/ios_private/configure_project.py
--mode all` ile `win32_ext.c`/`com.c` arka ucunu kullanır. Baz `ios/project.yml`
probe kaynaklarını üretim runtime'ı sanmamak gerekir.

## Cihazda bekleyen kabul

Kaynak incelemesi cihaz kanıtı değildir. Sonraki kullanıcı isteğiyle
hazırlanacak build üzerinde aşağıdakiler doğrulanmalı:

1. Rachel başlangıç bilgilendirmesi touch Enter/Geri/Esc ve DS4 ile geçer;
   menü ve yarış girişleri çift aksiyon veya kayıp kısa basış üretmez.
2. Ayarlar açıkken ve arka plan/ön plan, disconnect/reconnect veya aktif kol
   değişiminde takılı tuş ve sızan input kalmaz; ilk yeni basış kabul edilir.
3. Desteklenen DS4 bağlantısında kısa titreşim, şiddet ve kapatma gerçekten
   kol motorunda çalışır; desteklenmeyen bağlantı açıkça bildirilir.
4. Temiz profilde gerçek native render ve full LOD seçimi; kayıtlı profilde
   kullanıcının çözünürlük/detay tercihlerinin korunması teyit edilir.

Build34 için kullanıcının daha önce bildirdiği native/full LOD/sabit 60 FPS,
bu yeni kaynakların cihaz doğrulaması sayılmaz. GPUVS, scheduler, saat,
PCM ve renderer optimizasyonlarına bu çalışmada dokunulmadı.
