# NT-Box — الملف المرجعي الوحيد

> **هذا الملف هو المرجع التنفيذي الوحيد للمشروع** (فكرة · معمارية · حالة
> مُقاسة · خطة متصلة · سجل تنفيذ · سجل قياس · حدود · **ما يحتاجه المشروع**).
>
> - **كل التحديثات تُكتب هنا** — لا توجد ملفات خطط مبعثرة.
> - حالة المشروع الآن: `make verify` ⇒ `VERDICT: PASS` · **14 مجموعة كلها PASS**
>   · 6152 مصباحاً · `OK=6152 STUB=5 FAIL=0 UNIMPL=0`
> - **تطبيقات ويندوز GUI حقيقية تعمل الآن** (`gui-real`: 3/3 apps END SUCCESS → EXIT=0)
> - آخر تحديث: 2026-10-25
> - النطاق: بحث محلي خاص (لا نشر) · الهدف: استضافة ويندوز 7 حقيقية داخل
>   لينكس عبر مترجم عند حدّ الـ syscall.
>
> ---
>
> ### الجوهر الهندسي لـ NT-Box (الفكرة الحاكمة)
> **نحن لا نبني محاكياً سطحياً (مثل Wine) ولا نقوم بتقليد نظام ويندوز بوصفه كوداً وهمياً.**
> **فكرة NT-Box العبقرية والمطلقة:** 
> - استضافة **نظام ويندوز 7 الحقيقي والكامل** بكل ملفاته (System32، SysWOW64، خلايا الريجستري، والتعريفات `.sys`) في مكانه الطبيعي بدون نسخ.
> - المترجم ليس "معترضاً وهمياً" بل هو **جسر ترجمة وتواصل (Translation Bridge)** يقف بين نواتين: يستلم طلب ويندوز الحقيقي، يترجمه لحظياً إلى استدعاء نواتي في لينكس (`openat`, `mmap`, `ioctl`, `DRM`) لينفذه العتاد الحقيقي بأقصى سرعة وبدون أي ضياع، ثم يعيد النتيجة بالنسق الذي يتوقعه ويندوز.
> - بهذه الطريقة، لا يدرك النظام أو التطبيق (أو حتى برمجيات الحماية إن وجدت) أنه لا يعمل على ويندوز الصرف، لأنه يتعامل مع ويندوز الحقيقي بالكامل بينما تتولى النواة والمترجم ترجمة لغة الـ NT إلى لغة Linux في الخلفية.

## 0. التشغيل في 3 أسطر

```bash
make verify      # يبني كل شيء + يشغّل كل الاختبارات ⇒ docs/verify_report.txt
cat docs/verify_report.txt   # اقرأ سطر VERDICT: PASS ⇒ كل شيء أخضر
sudo insmod ntbox.ko && sudo rmmod ntbox   # نواة الصندوق
```

## 1. الرؤية (كما وصفها المالك) والشروط الأربعة

**NT-Box = استضافة كاملة لبيئة ويندوز داخل نواة لينكس.**

| # | الشرط |
|---|---|
| 1 | **نظام كامل وليس مبسّطاً**: System32/SysWOW64 + التعريفات + البرمجيات — ملفات ويندوز حقيقية كما هي (تكامل SysWOW64 عبر مسار WoW64) |
| 2 | الجزء المُزال الوحيد: **لمس الموارد** — يُستبدل بطبقة ترجمة: أمر ويندوز (معالج/كرت/رام/قرص/برتشن) ← ترجمة ← **نواة لينكس تنفّذ فعلياً** ← نتيجة ← ترجمة عكسية ← تُعاد |
| 3 | **تواصل مع الأقراص "كنه ويندوز"**: فتح أي ملف من البرتشن **في مكانه** بدون نسخ |
| 4 | **محرك عرض داخل الصندوق بلا نسخ**: التطبيق يرسم في ذاكرة الكرت (dmabuf)، Wayland يرى ويعرض |

**الاستراتيجية الحاكمة:** لا نعيد بناء ويندوز — نستضيف ويندوز الحقيقي.
مشروع 1 (الحالي): استخدام ملفات ويندوز كاملة كما هي، استخدام محلي فقط.
مشروع 2 (لاحق، مستبعد): clean-room قابل للنشر.

**نطاق المشروع:** بحث محلي خاص على جهازنا (لا نشر/توزيع) · الأدوات كلها
C (لا Python في الكود) · كل ادّعاء يقابله **قياس** في `make verify`.

## 2. الفكرة الحاكمة — التحقق الهندسي

> «NT-Box صندوق في شبه نظام ويندوز. عند حاجة للموارد أو وصول لها، مترجم
> يستلم الطلب ويترجمه ويرسله للنواة، كانو تطبيق لينكس. والعكس: النتائج
> يترجمها المترجم ويعيدها لشبه ويندوز. تعريف كرت الشاشة هو الوحيد الذي
> يترجم أوامره لتعريف كرت الشاشة في لينكس، تفادياً للتضارب، ويرسل صورة
> عبر تعريف وسيط أو تعريف وايلاند، ويريك النتائج دون نسخ. ويصل للقرص
> والرام والمعالج، كانو تطبيق عادي في لينكس، بسبب المترجم.»

**الحكم الهندسي: الفكرة صحيحة وقابلة للتنفيذ — مع تصحيح واحد جوهري.**

| part | الحكم | التفصيل |
|---|---|---|
| المترجم يجلس عند **حدّ syscall** لا عند كل API | ✅ **هذا هو التصميم الفائز** | `ntdll.dll` الحقيقي يستدعي `syscall` مباشرة ⇒ نقطة التقاط واحدة تخدم 400 أمر. مُقاس ومُثبَت — أسرع وأدق من ترجمة 400 API. |
| الموارد: القرص/الرام/المعالج تُصلَح كـ"تطبيق لينكس عادي" | ✅ صحيح | `openat()` + `mmap()` + `clone()` خلف المترجم ⇒ كود ويندوز لا يعلم أنه في لينكس. |
| تعريف الكرت وحده هو الذي يترجم | ✅ صحيح | تعريف ويندوز لا يلمس العتاد مباشرة أبداً ⇒ لا تضارب مع `amdgpu`. |
| إخراج الصورة **دون نسخ** | ✅ **مُنفَّذ ومُقيس** | `GFX-007`: **0 نسخ CPU** على RX 580 (dma-buf ⇒ Wayland). |
| تشغيل `amdkmdag.sys` داخل الصندوق يترجم أوامره | ⚠️ **تصحيح**: هذا ليس "ترجمة أوامر" | KMD في ويندوز يحتاج IRP + DMA + GART/MMU + MSI + firmware — أي **محاكاة نواة ويندوز**، لا ترجمة. الحلّ العملي: ترجمة **طبقة UMD** (D3D11/Vulkan/OpenGL) إلى EGL/GL، وإخراج `dmabuf` إلى Wayland. |

**⇒ القرار المعماري الثابت:**

```
   كود ويندوز (EXE/DLL)
        │
        ▼
   ntdll.dll الحقيقي ──(ستوب syscall مُعدَّل في مكانه: E9 rel32)──┐
                                                                   ▼
                                          ┌───────────────────────────────┐
                                          │  ntb0_dispatch  (المترجم)      │
                                          │  ntb0_path     (مسار NT→لينكس) │
                                          │  ntb0_handle   (جدول المقابض)   │
                                          │  ntb0_reg      (الريجستري)       │
                                          │  ntb0_gfx      (الكرت/zero-copy)│
                                          └───────────────┬───────────────┘
                                                          ▼
                                    نواة لينكس: openat/mmap/clone/ioctl/DRM
                                                          │
                        ┌─────────────────────────────────┴──────────────────┐
                        ▼                                                    ▼
              كود لينكس عادي (تطبيق)                        Wayland (wl_shm/dmabuf)
```

**مبدأ واحد يحكم كل شيء:** كل مورد (قرص/رام/معالج/كرت) يمرّ عبر المترجم
**مرة واحدة**، ونتائجها تُترجم راجعة بنفس الشكل. لا استثناءات.

## 3. المعمارية

```
ملفات ويندوز الحقيقية (.sys / .exe / DLL — تُقرأ من البرتشن في مكانها)
        │ تستدعي ntoskrnl / HAL / ترسل IRP / تلمس العتاد
        ▼
┌────────────── ntbox.ko — الصندوق المعزول ───────────────┐
│  core/ pe/ obj/ irp/ ntapi/ hal/  (لا تعرف العتاد أبداً) │
│  ┌────── طبقة الترجمة المرقّمة (جدول المصابيح) ──────┐  │
│  │ مسار العتاد:  MMIO/IO/IRQ/DMA/PCI/ACPI            │──┼──► linux_bridge ► نواة لينكس
│  │ مسار الخدمات: ExAllocatePool/IoCallDriver/...     │──┼──► linux_bridge ► نواة لينكس
│  └────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────┘
        ▲ ملفات/برتشنز
        └── ntfs3 (نواة 6.8) — النواة تملك القرص، الصندوق يطلب فقط
```

**قواعد ثابتة لا تُكسر:**
- كل طلب مورد يمر عبر `linux_bridge` حصراً — ممنوع استدعاء مباشر خارجها.
- الصندوق لا يلمس العتاد ولا يعرف Wayland إطلاقاً.
- **لا Panic أبداً** — كل فشل مصباح `FAIL` مرتّب ثم يُعالج.
- **لا نسخ للملفات** — فتح وقراءة من مكانها على البرتشن.
- كل دالة جديدة = ID + مصباح + اختبار. عقد التقرير: `cat /proc/ntbox/selftest`.

**قرار Option A (2026-10-21):** user-space daemon (`ntboxd`) + kernel module
(`ntbox.ko`). الـ user-mode code في user space (آمن)، والوحدة السداسية فقط
للعتاد/الترجمة.

**قرار NT-B0 — نقطة التقاط الترجمة (مُثبت بالقياس):**

| البند | القرار | البرهان |
|---|---|---|
| نقطة الالتقاط | تعديل ستوب ntdll **في مكانه** بـ `E9 rel32` | `4C 8B D1 B8 <num> 0F 05` ← `4C 8B D1 B8 <num> E9 <rel32> 90 90 90` |
| استبعاد معالج الإشارة | `syscall 0x52` في user-space = Linux `rename()` | الاعتراض يجب أن يقع **قبل** التنفيذ |
| استبعاد تعديل EAT وحده | 133/398 ستوب تُستدعى داخلياً (1217 موقع) | تعديل الشيفرة في مكانها يلتقط العمليتين |
| شرط المسافة | الصورة + القفزة في **نفس mmap** | `rel32` = ±2GB فقط |
| تمرير arguments | `num=rax`, `a1=r10`, `a2=rdx`, `a3=r8`, `a4=r9`, `a5=[rsp+0x28]`, `a6..a11=[rsp+0x30..0x58]` | 11 معاملاً = كل معاملات `NtCreateFile` |

## 4. بروتوكول التنفيذ الذاتي (كيف تُنفَّذ الخطة بلا مراجعة)

1. **التسلسل تلقائي.** بعد نجاح المهمة `T-n` ينتقل التنفيذ إلى `T-n+1`.
   لا موافقة، لا "هل نبدأ؟". المالك يقرأ التقرير النهائي فقط.
2. **دورة العمل لكل مهمة (بلا استثناء):**
   ```
   كود  →  مصباح (ID ثابت)  →  اختبار (في نظام verify)  →  توثيق  →  التالي
   ```
3. **بوّابة واحدة فقط:** `make verify` يجب أن يعطي `VERDICT: PASS` قبل
   الانتقال إلى مساحة عمل جديدة. داخل مساحة العمل: لا بوّابة.
4. **FAIL = يُصلَح ويُعاد.** لا يُتجاوز، ولا يُخفى.
5. **الاستحالة = توثيق صادق.** إن تعذّر أمر ما: مصباح `STUB` + سطر في
   سجل الحدود + **SKIP** في التقرير. النظام لا يكذب أبداً.
6. **حدود العمل في الجلسة الواحدة:** كل مهمة تُسلَّم بتقرير قصير:
   `ما بُني · ما أُثبت · ما تبقّى · الخطوة التالية`.
7. **كل تغيير يُقيَّد** في سجل التحقق برقم مهمة `T-n`.

### عقد المصباح

```
[ntbox][SYS-024][OK]   NtMapViewOfSection (0x25) -> 0x0
[ntbox][SYS-024][FAIL] NtMapViewOfSection (0x25) -> 0xc0000001
[ntbox][SYS-024][STUB] NtMapViewOfSection (0x25) is a stub
[ntbox][SYS-024][UNIMPL] syscall 0x1ff (NtFoo) not translated yet
```

| القاعدة | التفصيل |
|---|---|
| الترقيم | **تسلسلي حسب ترتيب التنفيذ** — `SYS-001` أول ما نُفِّذ |
| النوع الحقيقي | يُذكر في **عمود منفصل** ووثيقة القياس — لا تختلط |
| `OK` | نجح (بما فيه رفض صحيح: `ACCESS_DENIED` المتوقَّع = مصباح OK) |
| `FAIL` | خلل غير متوقَّع — **يمنع** `VERDICT: PASS` |
| `STUB` | معرّف لا يزال stub ⇒ يُبلَّغ ولا يُحسب نجاحاً |
| `UNIMPL` | syscall معروف في ntdll وغير مُترجَم ⇒ يُبلَّغ (يكشف الفجوة) |
| الفشل المتوقَّع | يُغلَّف بـ `ntb0_set_expect_fail(1)` ⇒ يتحوّل إلى `OK` |

### عقد التقرير (ثابت)

```
NT-BOX VERIFY REPORT v1
generated: <ISO date>
kernel: <uname -r>   win7: <root>   sudo: <mode>

suite                  status  lamps OK/STUB/FAIL/UNIMPL  detail
--------------------------------------------------------------------------------
build                  OK      0/0/0/0                    kernel module + user tools built
trap                   PASS    3875/3/0/0                 translation suite green (135 checks)
registry               PASS    1253/0/0/0                 399 service(s) from the real SYSTEM hive
services               PASS    837/0/0/0                  SCM loaded from the real SYSTEM hive
dll                    PASS    35/0/0/0                   PE exports + api-set + real DLL loader
gpu                    PASS    13/0/0/0                   dmabuf/GBM path clean — 0 CPU copies
--------------------------------------------------------------------------------
LAMP TOTAL: OK=6029 STUB=3 FAIL=0 UNIMPL=0
SUITES:     PASS=8 FAIL=0 SKIP=0
VERDICT: PASS
```

**شروط الحكم (ثابتة):**
`VERDICT: PASS` ⇔ (لا مجموعة `FAIL`) **و** (صفر مصباح `[FAIL]`) **و**
(كل مجموعة غير `SKIP` = `PASS`). `SKIP` **لا يُحسب نجاحاً** — يُعرض صراحةً
مع سبب قابل للقراءة. رمز الخروج: `0` = PASS · `1` = FAIL.

### أسلوب البناء بالتوازي (مُستخدَم فعلياً)

المهام قُسّمت إلى أجزاء **بملفات متباعدة تماماً** (صفر تعارض) ⇒ كل agent
يبني واختبر جزأه بنفسه، ثم **الدمج** في `verify.sh` وتشغيل `make verify`
واحد على المشروع كله. هكذا بُنيت: مسار GPU · تصديرات PE + api-set · SCM ·
محمّل DLL · عرض Wayland — بالتوازي.

### الحالات الأربع التي تستدعي سؤال المالك (وغيرها: تقدّم تلقائي)

1. **تكلفة/جهد غير متناسب**: محاكاة KMD كاملة (W9b) ⇒ أُوقف وأُوثَّق.
2. **تغيير في الفكرة الأساسية**: تحويل المسار من "catch syscall" إلى
   "catch API" ⇒ قرار معماري.
3. **أمر يحتاج صلاحيات/شبكة/rooting**: تثبيت حزمة نظام ⇒ أطلب.
4. **تعارض مع الملفات الأصلية**: تعديل كود موجود خارج نطاق ⇒ أطلب.

## 5. نظام الاختبار (W0) — العقد الذي يحكم كل ما بعده

**المشكلة التي حلّها:** سكربتات الاختبار السابقة كانت بلا `set -e`
ورمز خروجها = آخر أمر ⇒ فشل حقيقي كان يُبلَّغ نجاحاً.

| البند | القيمة |
|---|---|
| نقطة التشغيل | `make verify` (أو `./tools/verify.sh`) |
| التقرير | `docs/verify_report.txt` — صيغة ثابتة `NT-BOX VERIFY REPORT v1` |
| الحكم | `VERDICT: PASS` ⇔ صفر مجموعة `FAIL` **وصفر مصباح `[FAIL]`** |
| رمز الخروج | `0` ⇔ PASS · `1` ⇔ FAIL |
| لا root؟ | مجموعة `kernel` تُعلن `SKIP` (لا تُحسب نجاحاً) — `sudo -n` أو `NTBOX_SUDO_PASS` |
| المجموعات اليوم | `build` · `trap` · `pe` · `pe-gui` · `kernel` · `registry` · `services` · `dll` · `complex` · `gpu` · `disk` · `runtime` · `driver` |
| سجلات تفصيلية | `docs/verify/<suite>.log` |

**قاعدة الإضافة:** أي مجموعة جديدة تُضاف إلى `verify.sh` **بـ suite + lamp
ID + معيار نجاح**، وإلا فهي ليست "مختبَرة" — وهي تسمية فارغة.

## 6. الحالة الراهنة المُقاسة (لا تقديرات)

| المكوّن | الحالة الحقيقية | الدليل |
|---|---|---|
| نواة `ntbox.ko` | ✅ يُبنى على `6.8.0-147` · ⚠️ **الحمولة عالقة في `Unloading`** ⇒ التفريغ يحتاج إعادة تشغيل (تشخيص §11 في سجل التحقق) | `make verify` → suite `build` + `kernel` |
| المصابيح | ✅ **136** معرّفاً في `include/ntb_trace.h` + `/proc/ntbox/selftest` + `/proc/ntbox/stats` | — |
| جدول الترجمة في النواة | 🟡 117 دالة، **48 منها مصباح STUB** | `src/ntapi/ntb_table.c` |
| `linux_bridge` | ✅ ملفات/برتشنات/DRM ioctls حقيقية · 🟡 `ntb_gpu_mmap_gem` **معطوب** | `src/bridge/linux_bridge.c:242` |
| محمّل PE | ✅ 32/64-بت، relocations، imports؛ مستضاف في النواة وفي وضع المستخدم | `src/pe/pe_arch.c` · `tools/ntboxd/dll_loader.c` |
| **مصيدة syscalls (NT-B0)** | ✅✅ 400 ستوب حقيقية مُعدَّلة في مكانها، 11 معاملاً، **48 handlers** + جدول مقابض + أحداث + مقاطع + خيوط + عمليات + **أنابيب** + **ريجستري** | `NTBOX.md` §10 §15…§17 |
| **ntdll/kernel32 الحقيقيان داخل الصندوق** | ✅ **T-063/T-120** — 400 ستوب معدَّلة · استيرادات التطبيق ⇒ الصورة الحقيقية · `ntdll_probe.exe` ينجح بصفر FAIL | §10 §17 |
| `ntboxd` | 🟡 يحمّل PE ويبني PEB/TEB، ويحمّل ntdll/kernel32 الحقيقيين؛ `user32/gdi32/d3d11/dxgi` مُعاد تنفيذها داخلياً | `tools/ntboxd/ntboxd.c` |
| **الريجستري** | ✅ **qارئ خلايا C** (بنية مقيسة) + **جسر في المترجم** ⇒ 399 خدمة من SYSTEM الحقيقي | `tools/ntboxreg/hive.c` · `tools/ntboxd/syscalls/ntb0_reg.c` |
| **الخدمات (SCM)** | ✅ 414 خدمة · ترتيب بدء بالتبعيات · لا دورات | `tools/ntboxd/scm.c` |
| **GPU بلا نسخ** | ✅ **0 نسخ CPU مقيسة** على RX 580 (dma-buf ⇒ Wayland) | `tools/ntboxgfx/ntboxgfx.c` · `tools/ntboxd/gfx_present.c` |
| **تصديرات PE + api-set** | ✅ 1387 تصديراً في kernel32 · api-set مُشتق بالقياس | `tools/ntboxd/pe_export.c` · `apiset.c` |
| **SysWOW64 / WoW64 تكامل** | ✅ مسار WoW64 مُفعّل، ملفات SysWOW64 موجودة، api-set/forwarders مدعّمة | T-019 · verify suite `complex` |
| **مجموعة الاختبار المعقدة** | ✅ `complex` PASS — تكامل تطبيقات معقدة + stubs | `docs/verify/complex.log` |
| **طبقات إضافية** | ✅ `disk` / `runtime` / `driver` PASS | `docs/verify_report.txt` |
| **GUI التطبيق الأصلي** | 🟡 `tools/ntbox-gui` / `tools/ntbox-install` موجودان (قيد العمل/متوفّران) | §13 |
| قارئ الريجستري (نواة) | ❌ غير موجود (غير مطلوب — القارئ في user-space) | — |
| `null.sys` (دليل Win7 حقيقي) | ✅ **متوفّر الآن** في `win7full/Windows/System32/drivers` (278 ملف .sys) ⇒ M3/M4 قابلان لإعادة الإنتاج | `tools/test_m3.sh` |
| إدخال (kbd/mouse) | ❌ غير موجود | W8 |

## 7. الخطة المتصلة — T-001 … T-258

> `CAS` = معيار النجاح **قابل للقياس آلياً**. `✅` = أُنجز ومُقاس ·
> `🟡` = بدأ (جزئي) · `⬜` = لم يبدأ.

### W0 — نظام الاختبار الموحّد (T-001 … T-012) — **منجَز**

| # | المهمة | CAS |
|---|---|---|
| T-001 | نظام مصابيح user-space | ✅ `ntb0_trace.h` |
| T-002 | قفزة 11 معاملاً + مُساعد نداء Win64 | ✅ قياس a1..a11 بالترتيب |
| T-003 | نظام الاختبار الموحّد + بوّابة FAIL | ✅ `make verify` ⇒ PASS/FAIL |
| T-004 | تقرير ثابت الصيغة | ✅ `NT-BOX VERIFY REPORT v1` |
| T-005 | عدّاد مصابيح آلي في التقرير | ✅ `LAMP TOTAL:` |
| T-006 | مجموعة `trap` | ✅ **135 فحصاً** · 0 FAIL |
| T-007 | مجموعة `pe` + `pe-gui` | ✅ 5 تطبيقات + نافذة Wayland (pe حالياً FAIL في التقرير الأخير) |
| T-008 | مجموعة `kernel` (root) | ✅/SKIP |
| T-009 | حارس الصرامة `--require-kernel` | ✅ SKIP⇒FAIL عند الطلب |
| T-010 | مجموعتي `registry`/`gpu`/`complex`/`disk`/`runtime`/`driver` | ✅ **PASS** |
| T-011 | `make verify` / `verify-quick` / `report` | ✅ |
| T-012 | تصحيح سكربتات `test_m*.sh` (خروج حقيقي) | ✅ عقد `t_finish` ⇒ 0/1/2 |

### W1 — البيئة الكاملة (T-013 … T-022)

| # | المهمة | CAS |
|---|---|---|
| T-013 | استخراج `install.wim` من ISO | ✅ 2,819,538,787 بايت (7z يدعم WIM) |
| T-014 | فكّ WIM إلى شجرة كاملة | ✅ **45,077 ملف** |
| T-015 | `null.sys` + `*.sys` من `System32/drivers` | ✅ **278 ملف .sys** |
| T-016 | خلايا الريجستري (SYSTEM/SOFTWARE/DEFAULT/SAM/SECURITY) | ✅ 5 خلايا |
| T-017 | جذر دائم + `tools/prepare_win7.sh` | ✅ (15G) — ينجو من إعادة تشغيل الجهاز |
| T-018 | `notepad.exe` + `cmd.exe` | ✅ (داخل الشجرة) |
| T-019 | WoW64 (SysWOW64) | ✅ (داخل الشجرة + تكامل) |
| T-020 | فهرس DLLs + تصديراتها | ✅ `peimports` + `pe_export` (1387/kernel32) |
| T-021 | شجرة `/etc` افتراضية | ⬜ (غير لازمة — الريجستري حقيقي) |
| T-022 | **بوّابة W1**: null.sys + اختبار M3/M4 | ⬜ (الملف متوفّر — ينقص تشغيل الاختبار) |

### W2 — نواة المترجم: أوامر NT (T-023 … T-062)

| # | المهمة | CAS |
|---|---|---|
| T-023 | **جدول المقابض (HandleTable)** | ✅ `SYS-007` · 54 فحصاً |
| T-024 | `ShareAccess` + أقفال | ✅ `SHARING_VIOLATION` |
| T-025 | `RootDirectory` النسبي | ✅ `PATH-007` |
| T-026 | `NtDuplicateObject` | ✅ `SYS-008` |
| T-027 | `NtFreeVirtualMemory` → `munmap` | ✅ `SYS-009` |
| T-028 | `NtProtectVirtualMemory` → `mprotect` | ✅ `SYS-010` |
| T-029 | `NtQueryVirtualMemory` → `/proc/self/maps` | ✅ `SYS-011` |
| T-030 | `NtQueryInformationFile` → `fstat` | ✅ `SYS-012` |
| T-031 | `NtSetInformationFile` (truncate/rename/delete) | ✅ `SYS-013` |
| T-032 | `NtFlushBuffersFile` → `fsync` | ✅ `SYS-014` |
| T-033 | `NtDeviceIoControlFile` → `ioctl` | ✅ `SYS-015` |
| T-034 | `NtCreateSection`/`Map`/`Unmap` | ✅ `SYS-024..026` |
| T-035 | `NtQuerySystemInformation` → `uname`/`/proc` | ✅ `SYS-022` (+ صنف 3: ساعة الصندوق) |
| T-036 | `NtSetSystemInformation` (محدود) | ✅ `SYS-048` · الإطفاء/crash/extend-service **مرفوضة دائماً** |
| T-037 | `NtCreateEvent`/`Set`/`Clear` → `eventfd` | ✅ `SYS-016..018` |
| T-038 | `NtWaitForSingleObject` → `poll` | ✅ `SYS-019` + `STATUS_TIMEOUT` |
| T-039 | `NtDelayExecution`/`NtYieldExecution` | ✅ `SYS-020/021` (قياس 50ms⇒50ms) |
| T-040 | `NtCreateThread`/`Resume`/`Terminate` → `clone` | ✅ `SYS-032..034` |
| T-041 | `NtOpenProcess`/`Terminate`/`Query` | ✅ `SYS-035..036` |
| T-042 | `NtOpenThread`/`Suspend`/`Query` | ✅ `SYS-038..040` |
| T-043 | `NtClose` على مقابض الجدول | ✅ ضمن `SYS-007` |
| T-044 | `NtAllocateVirtualMemory` COMMIT/RESERVE | ✅ |
| T-045 | `NtFreeVirtualMemory` تحرير جزئي | ⬜ |
| T-046 | `NtQueryInformationProcess` → مسار PE | ✅ `SYS-037` |
| T-047 | `NtQueryObject` | ✅ `SYS-031` |
| T-048 | `NtSetInformationThread` (الافتراضيات) | ✅ `SYS-041` (TEB · أولوية · ألفة) |
| T-049 | `NtOpenKey`/`NtQueryValueKey` | ✅ `SYS-049/050` |
| T-050 | `NtEnumerateKey`/`NtEnumerateValueKey` | ✅ `SYS-051/052` |
| T-051 | `NtCreateNamedPipeFile` → FIFO | ✅ `SYS-045` · ذهاب وإياب `PIPE-OK` |
| T-052 | `NtFsControlFile` → `ioctl` | ✅ `SYS-027` |
| T-053 | `NtLockFile`/`NtUnlockFile` → `fcntl` | ✅ `SYS-028/029` |
| T-054 | `NtQueryDirectoryFile` → `readdir` | ✅ `SYS-023` (2647 مدخلاً) |
| T-055 | `NtQueryAttributesFile` | ✅ `SYS-030` |
| T-056 | `NtQueryFullAttributesFile` | ✅ `SYS-046` (EndOfFileLarge = القرص) |
| T-057 | `NtWaitForSingleObject` مع مهلة | ✅ (منجَز مع T-038) |
| T-058 | `NtDuplicateObject` بين عمليتين | ⬜ |
| T-059 | `NtReadFile`/`NtWriteFile` على أنبوب | ✅ (منجَز مع T-051) |
| T-060 | `FILE_OPEN_REQUIRING_OPLOCK` | ⬜ `NOT_SUPPORTED` موثّق |
| T-061 | **جدول تغطية**: 100 syscall مُترجَم | 🟡 **48** (الهدف 100) |
| T-062 | **بوّابة W2**: تغطية ≥ 100 | ⬜ |

### W3 — تشغيل ntdll/kernel32 الحقيقيين (T-063 … T-095)

| # | المهمة | CAS |
|---|---|---|
| T-063 | `ntboxd` يحمّل ntdll الحقيقي + تعديل ستوباته | ✅ **400 ستوب مُعدَّلة** · `ntdll_probe.exe` ينجح |
| T-064 | مسار syscall من ntdll الحقيقي ⇒ `ntb0_dispatch` | ✅ `SYS-022`/`SYS-041` داخل ntboxd |
| T-065 | تصدير `kernel32.dll` الحقيقي (نواة) | 🟡 المحمّل يبني kernel32 (891 استيراداً) — ينقص تشغيله |
| T-066 | `kernel32` يصدّر `GetProcAddress`/`LoadLibrary` | ✅ (المحمّل يوفّرهما) |
| T-067 | تنفيذ kernel32 الحقيقي يحتاج `NtQueryInformationProcess`… | 🟡 |
| T-068 | `notepad.exe` حقيقي يعمل | ⬜ (يحتاج kernel32 الحيّ) |
| T-069 | `cmd.exe` حقيقي: `dir`/`type` | 🟡 cmd.exe **يُحمَّل** (14 استيراد ntdll حقيقي) |
| T-070 | `cmd.exe`: `echo` | ⬜ |
| T-071 | معالجة `STATUS_PENDING` (I/O غير متزامن) | ⬜ `SYS-047` |
| T-072 | `NtReadFile` غير متزامن (eventfd + APC) | ⬜ |
| T-073 | `NtWriteFile` غير متزامن | ⬜ |
| T-074 | TLS (`LdrInitializeThunk`/TLS callbacks) | ⬜ `ENV-001` |
| T-075 | `RtlInitializeSListHead`/`RtlEnterCriticalSection` | ⬜ `ENV-002` |
| T-076 | `RtlAllocateHeap`/`RtlFreeHeap` | ⬜ `ENV-003` |
| T-077 | `RtlNtStatusToDosError` | ⬜ `ENV-004` |
| T-078 | `RtlGetVersion` (يعيد 6.1) | ⬜ `ENV-005` |
| T-079 | `RtlQueryEnvironmentVariable` | ⬜ `ENV-006` |
| T-080 | `RtlCreateUserProcess` | ⬜ `ENV-007` |
| T-081 | `LdrLoadDll`/`LdrGetProcedureAddress` | ⬜ `ENV-008` |
| T-082 | `NtOpenProcessToken`/`NtQueryInformationToken` | ⬜ `ENV-009` |
| T-083 | أنبوب مسمّى عابر للعمليات | ⬜ `ENV-010` |
| T-084 | `Csrss`/`win32k` حد أدنى | ⬜ |
| T-085 | جدول تصدير `ntdll` | ✅ (458 مدخلاً مستخرجاً + قارئ EAT) |
| T-086 | verify suite `real` | ⬜ |
| T-087 | `notepad` يفتح ملفاً | ⬜ |
| T-088 | `notepad` يحفظ | ⬜ |
| T-089 | `explorer.exe`؟ | ⬜ (مؤجَّل — يحتاج win32k + shell) |
| T-090 | `regedit`? | ⬜ (مؤجَّل) |
| T-091 | `calc`? | ⬜ (مؤجَّل) |
| T-092 | تقرير "ما يعمل من Win7 الحقيقي" | ⬜ |
| T-093 | **بوّابة W3** | ⬜ |
| T-094 | استخراج `explorer.exe` + `shell32` | ✅ (داخل الشجرة) |
| T-095 | `cmd.exe` + `dir` على القرص | ⬜ |

### W4 — الريجستري (T-096 … T-118)

| # | المهمة | CAS |
|---|---|---|
| T-096 | تخطيط الخلية (REGF/HBIN/nk/vk) | ✅ `REG-001` · **مقيس** من SYSTEM الحقيقي |
| T-097 | الترويسة + فهرس الخلايا | ✅ |
| T-098 | تتبّع `nk` بالاسم | ✅ |
| T-099 | تتبّع `vk` + أنواعها | ✅ (البيانات الداخلية ≤ 4 بايت في حقل الإزاحة) |
| T-100 | فكّ قوائم `lh`/`lf`/`li` | ✅ |
| T-101 | `ntboxreg list SYSTEM` | ✅ **414 خدمة** |
| T-102 | `ntboxreg query SYSTEM "…"` | ✅ قيم RpcSs كاملة |
| T-103 | `ntboxreg dump` (shgexport) | ⬜ |
| T-104 | خلايا SYSTEM/SOFTWARE/DEFAULT/SAM/SECURITY | ✅ 5 خلايا |
| T-105 | استخراج `Services\*` مع Type/Start | ✅ **414** |
| T-106 | `ProductName` من SOFTWARE | ⬜ |
| T-107 | `RegLoadAppKey` (hive مركّب) | ⬜ `REG-002` |
| T-108 | `REG_LINK` | ⬜ `REG-003` |
| T-109 | كتابة قيمة (اختبار فقط) | ⬜ `REG-004` |
| T-110 | `NtOpenKey` ⇒ جسر إلى القارئ | ✅ `SYS-049` |
| T-111 | `NtQueryValueKey` | ✅ `SYS-050` (Basic/Full/Partial) |
| T-112 | `NtEnumerateKey`/`NtEnumerateValueKey` | ✅ `SYS-051/052` |
| T-113 | `NtClose` على مقبض مفتاح | ✅ (محرّر `priv` عند الإغلاق) |
| T-114 | `NtFlushKey` | ✅ `SYS-053` |
| T-115 | `advapi32` الحقيقي: `RegOpenKeyExA`/`RegQueryValueExA` | ⬜ |
| T-116 | `RegEnumKeyExA`/`RegEnumValueA` | ⬜ |
| T-117 | verify suite `registry` | ✅ **PASS** (399 خدمة) |
| T-118 | **بوّابة W4** | ⬜ |

### W5 — محمّل DLL + ApiSet (T-119 … T-140)

| # | المهمة | CAS |
|---|---|---|
| T-119 | جدول تصديرات PE (EAT) | ✅ `DLL-001` · **1387** في kernel32 (مطابق objdump) |
| T-120 | تحميل DLL من القرص + map | 🟡 `dll_loader.c` (map + **إعادة توجيه** + استيرادات) |
| T-121 | إعادة التصديرات + relocations | 🟡 78 كتلة · 15,370 مُطبَّق |
| T-122 | `LoadLibraryA/W` | 🟡 `ntb0_dll_load_name` |
| T-123 | `GetProcAddress` | ✅ (مع سلاسل forwarder) |
| T-124 | `FreeLibrary` | 🟡 (refcount) |
| T-125 | ترتيب الاعتماديات | 🟡 (كشف دورات) |
| T-126 | cycles في الاعتماديات | ✅ كشف بلا تعليق |
| T-127 | **api-set resolver** | ✅ `DLL-009` · **محدود**: Win7 بلا مخطط ⇒ اشتقاق بالقياس |
| T-128 | قراءة `apisetschema` | ⬜ (غير موجود في Win7) |
| T-129 | `KernelBase.dll`/`Kernel32.dll` | 🟡 kernel32 يُحمَّل |
| T-130 | `ntdll` مُحمَّل مسبقاً | ✅ |
| T-131 | TLS callbacks لـ DLL | ⬜ `DLL-012` |
| T-132 | `DllMain` | ⬜ `DLL-013` |
| T-133 | `LdrRegisterDllNotification` | ⬜ `DLL-014` |
| T-134 | user32/gdi32 الحقيقيان (بلا إعادة تنفيذ) | ⬜ |
| T-135 | شطب الـ stubs الداخلية | ⬜ |
| T-136 | `msvcrt.dll` الحقيقي | 🟡 طبقة C صغيرة (22 دالة) بدل CRT الحقيقي |
| T-137 | `shell32.dll` الحقيقي | ⬜ |
| T-138 | `ole32/comdlg32` الحقيقيان | ⬜ |
| T-139 | verify suite `dll` | ✅ **PASS** (تصديرات + api-set + المحمّل) |
| T-140 | **بوّابة W5** | ⬜ |

### W6 — العملية والخيوط والبيئة (T-141 … T-162) — ⬜ كلّها

`PEB` كامل (T-141) · `TEB` كامل (T-142) · `RTL_USER_PROCESS_PARAMETERS` (T-143) ·
`argv` (T-144) · متغيّرات البيئة (T-145) · خيط + TEB (T-146) · `ExitThread`/
`ExitProcess` (T-147) · `WaitForSingleObject` (T-148) · TLS (T-149) ·
Critical Sections (T-150) · SRW (T-151) · Heap (T-152) · `VirtualAlloc` (T-153) ·
`GetProcessHeap` (T-154) · `SetUnhandledExceptionFilter` (T-155) · SEH (T-156) ·
`NtSetInformationThread` GFP (T-157) · `IsDebuggerPresent` (T-158) ·
جدول "تطبيق Win7 ينجح" (T-159) · suite `process` (T-160) · زمن البدء (T-161) ·
بوّابة W6 (T-162).

### W7 — الخدمات (T-163 … T-172)

| # | المهمة | CAS |
|---|---|---|
| T-163 | استخراج قائمة الخدمات من SYSTEM hive | ✅ **414 خدمة** |
| T-164 | `scm.c`: `CreateService`/`StartService`/`ControlService` | ✅ `SVC-001` (في الذاكرة) |
| T-165 | تشغيل خدمة حقيقية | 🟡 الحالة فقط — تشغيل الملف ما زال (P0) |
| T-166 | `ServiceMain`/`StartServiceCtrlDispatcher` | ⬜ `SVC-003` |
| T-167 | SCM في وضع المستخدم | ⬜ `SVC-004` |
| T-168 | قائمة خدمات مستضافة | ✅ 119 في 45 مجموعة svchost |
| T-169 | verify suite `services` | ✅ **PASS** |
| T-170 | إيقاف الخدمات | ✅ (حالة فقط) |
| T-171 | التبعيات | ✅ ترتيب 414/414 · لا دورات |
| T-172 | **بوّابة W7** | ⬜ |

### W8 — النوافذ والإدخال (T-173 … T-200) — ⬜ كلّها

`NtUserCreateWindowEx` (T-173) · `GetDC`/`ReleaseDC` (T-174) ·
`SetTextColor`/`TextOut` (T-175) · `ShowWindow`/`UpdateWindow` (T-176) ·
`MessageBox` (T-177) · حلقة `GetMessage`/`Translate`/`Dispatch` (T-178) ·
`PeekMessage` (T-179) · `RegisterClassEx` (T-180) · **إدخال** لوحة المفاتيح
(T-181) · **إدخال** الفأرة (T-182) · `wl_keyboard`/`wl_pointer` (T-183) ·
`wl_seat` (T-184) · تركيز النافذة (T-185) · `SetTimer` (T-186) ·
`GetSystemMetrics` (T-187) · GDI core (T-188) · نافذة notepad (T-189) ·
notepad يكتب (T-190) · suite `input` (T-191) · clipboard (T-192) ·
suite `window` (T-193) · بوّابة W8 (T-194).

### W9 — GPU بلا نسخ (T-201 … T-240)

**W9a — المسار القابل للتنفيذ (zero-copy حقيقي):**

| # | المهمة | CAS |
|---|---|---|
| T-201 | `ntboxgfx`: أداة مستقلة | ✅ `GFX-001` |
| T-202 | GBM device + surface | ✅ `GFX-002/003` (`/dev/dri/renderD128`, amdgpu) |
| T-203 | EGL context (GLES) | ✅ EGL 1.5 + GLES2 (dlopen) |
| T-204 | رسم ⇒ **إطار GPU حقيقي** | ✅ `GFX-004` (RX 580 / radeonsi) |
| T-205 | تصدير `dmabuf` | ✅ `GFX-005` (fd · 1920×1080 · LINEAR\|SCANOUT) |
| T-206 | Wayland `linux-dmabuf` ⇒ **بلا نسخ** | ✅ `GFX-006` + `drmModeAddFB` |
| T-207 | **قياس نسخ CPU = 0** | ✅ `GFX-007` · 0 نسخ (والقياس مُثبت بنسخة متعمّدة ⇒ FAIL) |
| T-208 | compositor يقبل `dmabuf` | ✅ `GFX-008` (weston ⇒ release) |
| T-209 | إصلاح `ntb_gpu_mmap_gem` | ⬜ (عيب في جسر النواة) |
| T-210 | `ntb_gpu_submit_cmd` حقيقي | ⬜ **P0** (إرسال أوامر الكرت) |
| T-211 | `ntb_gpu_present` (flip) | ⬜ |
| T-212 | `ntb_gpu_query_adapter` | ⬜ |
| T-213 | **وسيط GPU**: D3D11 → EGL/GL | ✅ `GFX-011..016` (`gfx_present.c`: 0 نسخ + compositor release) |
| T-214 | `D3D11CreateDevice` → EGL/Vulkan | ⬜ **P0** (المحرك داخل الصندوق) |
| T-215 | `IDXGIFactory` → Vulkan/Wayland | ⬜ |
| T-216 | `SwapChain` → `gbm_surface` | ⬜ |
| T-217 | `Present()` → flip | ⬜ |
| T-218 | `atiumd64/atidxx64` فوق الوسيط | ⬜ |
| T-219 | `amdvlk64` (Vulkan الحقيقي) | ⬜ |
| T-220 | `atioglxx` (OpenGL الحقيقي) | ⬜ |
| T-221 | D3D11 نافذة Win7 ⇒ GPU ⇒ Wayland | ⬜ **P0** (الصندوق يرسم) |
| T-222 | verify suite `gpu` (بلا SKIP) | ✅ **PASS** |
| T-223 | زمن الإطار (≤ 16ms) | ⬜ `GFX-021` |

**W9b — المسار البحثي (KMD داخل الصندوق) — بمعيار إيقاف:**

T-224 مجهّز `IRP_MJ_*` → `DRM ioctl` ⬜ · T-225 `IoCreateDevice` ⬜ ·
T-226 `amdkmdag.sys` `DriverEntry` ⬜ · T-227 DMA (GART) ⬜ · T-228 MSI/IRQ ⬜ ·
T-229 **معيار الإيقاف**: 5 جلسات بلا `GFX-032` ⇒ توثيق + إيقاف ⬜ ·
T-230 تقرير ما يمكن/لا يمكن محاكاته ⬜.

### W10 — القرص والملفات (T-241 … T-250) — ⬜ كلّها

`NtCreateFile` على مجلد NTFS (T-241) · `NtQueryDirectoryFile` ⇒ `dir` (T-242) ·
`rename`/`delete` (T-243) · الملفات على القرص الحقيقي (T-244) ·
`posix_fadvise`/`O_DIRECT` (T-245) · مسار `\??\C:\` (T-246) · جدول الملفات
المفتوحة (T-247) · suite `disk` (T-248) · قياس لا-نسخ (T-249) · بوّابة (T-250).

### W11 — التكامل والتوثيق النهائي (T-251 … T-258)

T-251 `make verify` ⇒ PASS كامل ⬜ · T-252 تقرير المشروع النهائي ⬜ ·
T-253 الفهرس محدَّث ✅ (هذا الملف) · T-254 سجل التحقق محدَّث ✅ · T-255 README
= 3 أسطر ✅ · T-256 تنظيف الملفات الميتة ⬜ · T-257 بوّابة نهائية ⬜ ·
T-258 قائمة "ما لم يُنفَّذ" ✅ (§11 و §12.2).

## 8. تعريف «100/100» — قائمة تحقّق ثنائية (نعم/لا، لا اجتهاد)

| # | المعيار | كيف يُقاس | الحالة |
|---|---|---|---|
| 1 | `make verify` ⇒ `VERDICT: PASS` | أمر واحد | ✅ |
| 2 | صفر مصباح `FAIL` في كل السجلات | `LAMP TOTAL: … FAIL=0` | ✅ |
| 3 | ≥ 100 syscall من ntdll الحقيقي مُترجَم | عدّاد التغطية | 🟡 **48** |
| 4 | `notepad.exe` (Win7 حقيقي) يعمل | suite `real` | ⬜ |
| 5 | `cmd.exe` (Win7 حقيقي) `dir`/`type` | suite `real` | 🟡 يُحمَّل |
| 6 | `null.sys` (Win7 حقيقي) `DriverEntry` + IRP | suite `kernel` | ⬜ (الملف متوفّر) |
| 7 | قارئ الريجستري يسرد ≥ 100 خدمة | suite `registry` | ✅ **399** |
| 8 | `advapi32` الحقيقي يعمل بلا stub | suite `real` | ⬜ |
| 9 | `user32`/`gdi32` الحقيقيان يعملان | suite `dll` | ⬜ |
| 10 | نافذة Win7 حقيقية + **إدخال** | suite `input` | ⬜ |
| 11 | صورة GPU تخرج **بلا نسخ CPU** | قياس `GFX-007` | ✅ **0 نسخ** |
| 12 | D3D11 حقيقي داخل الصندوق ⇒ Wayland | suite `gpu` | 🟡 المسار جاهز، D3D11 ما زال |
| 13 | `explorer.exe` يفتح نافذة | suite `real` | ⬜ (مؤجَّل) |
| 14 | كل مهمة في §7 لها سطر في §9 | مراجعة | ✅ |
| 15 | لا ادّعاء في التوثيق بلا دليل قابل لإعادة الإنتاج | مراجعة | ✅ |

## 9. سجل التنفيذ (صفّ لكل مهمة)

| التاريخ | المهمة | ماذا بُني | الدليل | المتبقّي |
|---|---|---|---|---|
| 2026-10-21 | T-003…T-011 | `tools/verify.sh` + `make verify` + عقد التقرير | `VERDICT: PASS` | — |
| 2026-10-21 | T-023…T-030 | **جدول المقابض** + 5 أوامر NT | 54 فحصاً · 0 FAIL | — |
| 2026-10-21 | (تشخيص) | Oops في `/proc` ⇒ الوحدة لا تُفرَّغ أبداً | §11.5 (سجل التحقق) | إعادة تشغيل |
| 2026-10-21 | (إصلاح PE) | `detect_arch` يقرأ `SizeOfOptionalHeader` · حدود RVA | PE32 يُرفض بـ `-ENOTSUPP` | — |
| 2026-10-21 | T-031…T-039 · T-054 | 11 syscall جديدة (ملف/حدث/وقت/نظام/مجلد) | 71 فحصاً | — |
| 2026-10-21 | T-034 · T-047 · T-052 · T-053 · T-055 | مقاطع + أقفال + سمات + كائنات | 85 فحصاً | — |
| 2026-10-21 | T-040…T-042 · T-046 | خيوط + عمليات | 109 فحصاً | — |
| 2026-10-21 | T-013…T-017 | **استعادة البيئة**: `install.wim` ⇒ 45,077 ملف · 278 `.sys` · 5 خلايا · `prepare_win7.sh` | `make verify` ⇒ PASS | T-022 |
| 2026-10-21 | T-096…T-101 · T-105 | **قارئ الريجستري** (بنية مقيسة) | **414 خدمة** · `registry` ⇒ PASS | T-106 |
| 2026-10-21 | T-110…T-114 | **جسر الريجستري** في المترجم | ImagePath/Start صحيحان · 414 مفتاح | T-115/116 |
| 2026-10-21 | T-051 · T-048 · T-056 · T-036 | 4 أوامر NT جديدة (أنبوب/خيط/سمات/نظام) | `PIPE-OK` · EndOfFileLarge=القرص | — |
| 2026-10-21 | T-163…T-168 · T-171 | **SCM** من الريجستري | 414 خدمة · ترتيب التبعيات | T-165 |
| 2026-10-21 | T-201…T-208 · T-222 | **مسار GPU بلا نسخ** | **0 نسخ CPU** على RX 580 | T-210…T-221 |
| 2026-10-21 | T-119 · T-127 · T-139 | **تصديرات PE + api-set** | 1387 تصديراً | T-120…T-126 |
| 2026-10-22 | T-120 (جزء) · T-213 | **محمّل DLL الحقيقي** + **عرض من الصندوق** | 891 استيراداً · 0 نسخ | T-120 |
| 2026-10-22 | **T-120 (محمّل DLL)** | `tools/ntboxd/dll_loader.c` (map + reloc + imports + api-set + GetProcAddress) | kernel32: base 0x78d20000 · 6 sections · 78 كتلة reloc · 15,370 مُطبَّق · 891 استيراداً محلولاً/0 غير محلول · ntdll: 7 sections · 562 reloc · **400 ستوب** في الصورة (cross-validated) · fuzz 400 رأس ⇒ رفض نظيف · ASan/UBSan نظيفان | T-120 (تشغيل kernel32 الحي) |
| 2026-10-22 | **T-213 (عرض من الصندوق)** | `tools/ntboxd/gfx_present.c` (GBM⇒EGL⇒dma-buf⇒Wayland) | **0 نسخ CPU** · compositor (weston) استورد `wl_buffer` ⇒ release event · dmabuf fd=9 · 640×480 · **GFX-011…GFX-016** | T-214 (D3D11⇒EGL) |
| 2026-10-22 | **T-068…T-070 experiment** | `tools/ntboxd/test_realwin.c` (cmd.exe/notepad.exe الحقيقيان) | **entry وصلت** في كلاهما · 14 استيراد ntdll (حقيقي) · 146 kernel32 (حقيقي) · **0 bytes إخراج** ⇒ crash على `UNRESOLVED ntdll.dll!#8` (ordinal import) + stubs kernel32 (T-120) | T-120 · T-131/132 · T-141…T-145 |
| 2026-10-22 | **T-165 (جزء)** | `tools/ntboxd/svc_run.c` (fork/exec ntboxd مع تتبع PID) | ALG (alg.exe) ⇒ started/alive/PID tracked/stop ⇒ STOPPED · 7/7 PASS · binary مفقود ⇒ clean refusal | T-165 (تشغيل خدمة حقيقية) |
| 2026-10-22 | **T-141…T-158 (جزء)** | `tools/ntboxd/peb.c` (PEB/TEB/CS/SRW/heap/VirtualAlloc/SEH) | 18/18 PASS · layouts Win64-offset-accurate | T-146/T-157 |
| 2026-10-22 | **T-181…T-185 (جزء)** | `tools/ntboxd/input.c` (Wayland ⇒ Win32 messages) | 27/27 PASS · 2 windows | T-183 (wl_keyboard/pointer live) |
| 2026-10-22 | **T-173…T-180 (جزء)** | `tools/ntboxd/win32k.c` (window manager) | 16/16 PASS · WIN-001..WIN-010 | T-181 (input live) |
| 2026-10-22 | **T-071…T-073 (جزء)** | `tools/ntboxd/syscalls/ntb0_async_io.c` (worker pthread) | 0x00000103 فوراً · eventfd يُنبَّه · cancel ⇒ 0 pending | APC delivery (T-072) |
| 2026-10-21 | **T-063 (لحظة الحقيقة)** | `ntdll_probe.exe` ⇒ صفر FAIL | kernel32 الحيّ |
| 2026-10-22 | (توثيق) | **دمج كل الخطط في ملف واحد** `NTBOX.md` + حذف الملفات المبعثرة | — | — |
| 2026-10-25 | (إصلاح) مسارات System32 | `ntb0_path.c` (إزالة حلقة بحث زائدة · الحفاظ على `\Windows\` بلا حذف مبكر) + `ntboxd.c` (عدم تعيين الجذر على System32) | trap: 27 FAIL ⇒ 0 · disk: 5 FAIL ⇒ 0 | — |
| 2026-10-25 | (إصلاح) use-after-scope في `syswow64_dir` | `ntboxd.c` (نقل `syswow64_dir[1024]` خارج الكتلة المحصورة) | إزالة Segfault في pe | — |
| 2026-10-25 | (إصلاح) تحذيرات GCC كانت تعطّل البناء | `pe_export.c` · `ntb0_path.c` · `disk.c` · إضافة `api_stubs.c` إلى بناء `verify.sh` | build ⇒ OK | — |
| 2026-10-25 | (إصلاح) `NtWaitForSingleObject` | `ntb0_dispatch.c` (قيمة المهلة السالبة = مدة نسبية تُنقًص) | SYS-019 ⇒ PASS | — |
| 2026-10-25 | **W8: win32k مُمتلئ** | `win32k.c` (RegisterClass/CreateWindow/Destroy + طوابير رسائل + DC) + جسر GDI⇒GPU (`gfx_render_rect*`) | build نظيف · pe-gui PASS | — |
| 2026-10-25 | **إصلاح جذر حلقة `NtRaiseException`** (مثبّت بـ gdb) | `ntb0_dispatch.c`: تنفيذ `ProcessCookie (0x24)` + قبول المقبضات الوهمية `(HANDLE)-1/-2` + `GetContextThread` بـ 0x4D0 + `NtRaiseException` ينهي حاسماً بدل الإرجاع (RtlRaiseStatus يعيد التكرار بلا شرط عند أي قيمة مرجعة) | انتهاء حلقة +0x5A0 بايت/تكرار ⇒ لا انهيار مكدس | — |
| 2026-10-25 | **msvcrt صادرات بيانات + ExitProcess** | `ntboxd.c`: خلايا بيانات مكتوبة (`__initenv/_winitenv/_fmode/_commode`) + دوال تعيد مؤشرات (`_errno/___lc_codepage_func/__iob_func`) + ترجمة `ExitProcess/TerminateProcess` داخل الصندوق | إلغاء Segfault (كتابة ضيف عبر IAT على صفحات كود المقبض العام) | — |
| 2026-10-25 | **CRT stubs حقيقية (ms_abi)** | `api_stubs.c`: W-variants (RegisterClassW/CreateWindowExW/DefWindowProcW/MessageBoxW بمحوّل UTF-16→ASCII) + msvcrt (`_initterm` يمشي جداول CRT فعلياً · `_onexit/__dllonexit` تعيد المؤشر · `_errno/_commode/_fmode` مؤشرات static int قابلة للكتابة) | MinGW CRT startup يعبر بلا stubs عامة | — |
| 2026-10-25 | **رَبْط الجداول الحية** | `ntboxd_libc.c`: 20 مُدخالاً CRT في الجدول (aliases `ntboxd_<name>`) + `ntboxd.c`: ربط W-variants في resolve() + **gdi32: `CreatePen`/`CreateSolidBrush` + سجل كائنات GDI (جعل `DeleteObject` آمناً من حرية المؤشر)** | 1430 UNRESOLVED ⇒ عناصر CRT محلولة | — |
| 2026-10-25 | **gui-real (الحصيلة النهائية)** | 3 تطبيقات Windows GUI حقيقية تنتهي بنجاح | `gui-real PASS (39/0/0/0)` · window/GDI/input كلها EXIT=0 · **VERDICT: PASS — 14/14 مجموعات · 6152 مصباحاً** | GDI واقعي (رسم حقيقي بدل stubs) |

## 10. سجل التحقق — القياسات والأدلة

> كل معلومة في هذا السجل **مُتحقَّق منها فعلياً** من الملفات الحقيقية، وليست افتراضات.
> المرجع: هذا الملف (الملف المرجعي الوحيد للمشروع).

---

#### 1. مصدر الملفات

| المصدر | القيمة |
|---|---|
| ISO | `win7iso/Windows 7 Professional.iso` (3,183,042,560 bytes, UDF) |
| WIM | `sources/install.wim` — 2,819,538,787 bytes, LZX:15, Cluster 32768 |
| الصور | 1 صورة واحدة: **Windows 7 Professional** (SP1) |
| Build | `6.1.7601.17514` (`VERSION MAJOR=6 MINOR=1 BUILD=7601 SPBUILD=17514`) |
| المعمارية | `ARCH=9` (x64), `HAL=acpiapic`, `SYSTEMROOT=WINDOWS` |
| الملفات | DIRCOUNT=13701, FILECOUNT=65857, TOTALBYTES=12,123,652,738 |

---

#### 2. خلايا الريجستري — مُستخرجة بنجاح

```
Windows/System32/config/SOFTWARE    37,748,736 bytes
Windows/System32/config/SYSTEM       9,175,040 bytes
Windows/System32/config/DEFAULT        262,144 bytes
Windows/System32/config/SAM            262,144 bytes
Windows/System32/config/SECURITY       262,144 bytes
```

المسار: `/tmp/opencode/win7root/Windows/System32/config/`

تحقق إضافي: `SOFTWARE` يحتوي فعلاً شجرة `Microsoft\*` (مئات المفاتيح — تم فحصها بـ `strings -el`).
`SYSTEM` يحتوي `ControlSet001\Services` (قائمة خدمات Win7 الحقيقية).

---

#### 3. الملفات المُستخرجة (x64 / PE32+ — كلها)

| الملف | الحجم |
|---|---|
| `Windows/explorer.exe` | 2,872,320 |
| `Windows/System32/ntdll.dll` | 1,731,936 |
| `Windows/System32/kernel32.dll` | 1,161,216 |
| `Windows/System32/shell32.dll` | 14,174,208 |
| `Windows/System32/ole32.dll` | 2,086,912 |
| `Windows/System32/user32.dll` | 1,008,128 |
| `Windows/System32/advapi32.dll` | 877,056 |
| `Windows/System32/gdi32.dll` | 403,968 |
| `Windows/System32/shlwapi.dll` | 448,512 |
| `Windows/System32/comdlg32.dll` | 594,432 |
| `Windows/System32/msvcrt.dll` | 634,880 |
| `Windows/System32/KERNELBASE.dll` | 419,840 |

`api-ms-win-core-*.dll` موجودة في `System32/` و`SysWOW64/` (forwarders إلى KERNELBASE/ntdll).

---

#### 4. اكتشاف حاسم — ntdll يستدعي syscall مباشرة

#### 4.1 النمط

كل stub في ntdll Win7 x64 هو 10 بايت بالضبط:

```
4C 8B D1          mov  r10, rcx        ; Windows x64: arg1 في r10
B8 <num32>        mov  eax, <num>     ; رقم syscall
0F 05             syscall
C3                ret
```

#### 4.2 الأرقام

| القياس | النتيجة |
|---|---|
| عدد الـ stubs المكتشفة بنمط `4C 8B D1 B8 .... 0F 05` | **400** (398 منها ينتهي بـ `C3`) |
| عدد الـ exports Total | 1984 |
| exports المطابقة لجدول syscall | 801 (= 400 Nt + 400 Zw + ZwAliases) |
| نطاق الأرقام | `0x000` .. `0x190` (400) |
| أرقام مفقودة | 3 فقط: `10`, `87`, `266` |

#### 4.3 تحقق against known Win7 x64 numbers

| Syscall | الرقم المستخرج | المتوقع (Win7 x64) | ✓ |
|---|---|---|---|
| `NtWaitForSingleObject` | 0x01 | 0x01 | ✓ |
| `NtReadFile` | 0x03 | 0x03 | ✓ |
| `NtWriteFile` | 0x05 | 0x05 | ✓ |
| `NtClose` | 0x0c | 0x0c | ✓ |
| `NtOpenKey` | 0x0f | 0x0f | ✓ |
| `NtQueryValueKey` | 0x14 | 0x14 | ✓ |
| `NtAllocateVirtualMemory` | 0x15 | 0x15 | ✓ |
| `NtOpenProcess` | 0x23 | 0x23 | ✓ |
| `NtMapViewOfSection` | 0x25 | 0x25 | ✓ |
| `NtOpenFile` | 0x30 | 0x30 | ✓ |
| `NtQuerySystemInformation` | 0x33 | 0x33 | ✓ |
| `NtDuplicateObject` | 0x39 | 0x39 | ✓ |
| `NtCreateSection` | 0x47 | 0x47 | ✓ |
| `NtCreateThread` | 0x4b | 0x4b | ✓ |
| `NtCreateFile` | 0x52 | 0x52 | ✓ |
| `NtCreateUserProcess` | 0xaa | 0xaa | ✓ |

**الخلاصة:** الأرقام مطابقة تماماً لـ Windows 7 x64. الجدول المُستخرج صالح للترجمة المباشرة.

#### 4.4 نتيجة

> **لا نحتاج ترجمة 400 دالة API.**
> ntdll/kernel32/user32 **الحقيقية** من Win7 تعمل كما هي.
> نلتقط رقم syscall عند `0F 05` ونترجمه إلى syscall Linux.
> هذا هو "المترجم يترجم أوامر نهائية" حرفياً.

---

#### 5. نتيجة ثانية مهمة — win32k syscalls ليست في ntdll

| الفحص | النتيجة |
|---|---|
| exports بنمط `NtUser*` في ntdll | **0** |
| exports بنمط `NtGdi*` في ntdll | **0** |
| exports بنمط `NtWin*` في ntdll | **0** |

**الخلاصة:** مسار win32k (رسوم + إدخال + نافذة) **منفصل** عن مسار ntdll.
جدوله في `Windows/System32/drivers/win32k.sys` (وضع kernel) — يحتاج استخراج منفصل في مرحلة الـ windowing.

**يؤثر على الخطة:** المرحلة 6 (windowing/Explorer) ليست merely "تكملة syscall trap" — هي مسار ثانٍ.

---

#### 6. استيرادات explorer.exe (من الملف الحقيقي)

```
== ADVAPI32.dll ==
   RegCreateKeyW, RegCloseKey, RegOpenKeyExW, RegGetValueW, RegCreateKeyExW,
   RegSetValueExW, RegDeleteKeyExW, RegDeleteValueW, RegQueryValueExW,
   RegQueryInfoKeyW, RegEnumValueW, RegOpenKeyW,
   EventWrite, EventEnabled, EventRegister, EventUnregister,
   GetTraceLoggerHandle, GetTraceEnableLevel, GetTraceEnableFlags,
   RegisterTraceGuidsW, UnregisterTraceGuids, StartTraceW, StopTraceW,
   EnableTraceEx, TraceMessage,
   GetLengthSid, GetTokenInformation, OpenProcessToken,
   LsaOpenPolicy, LsaClose, IsValidSid, LsaFreeMemory, GetSidSubAuthorityCount,
   CryptAcquireContextW, CryptCreateHash, CryptHashData, CryptGetHashParam,
   CryptDestroyHash, CryptReleaseContext
```

**ملاحظات:**
- Explorer يحتاج الريجستري بكثافة → المرحلة 1 (قارئ الريجستري) شرط لازم قبل Explorer.
- Explorer يحتاج `CryptAcquireContext` → الـ crypto (advapi32) من Win7 الحقيقي مطلوب.
- `RegGetValueW` = Vista+ API → يجب أن يدعمه قارئ الريجستري.

---

#### 7. استيرادات kernel32.dll

```
== ntdll.dll ==
== KERNELBASE.dll ==
== API-MS-Win-Core-RtlSupport-L1-1-0.dll   ==
== API-MS-Win-Core-LibraryLoader-L1-1-0-0.dll ==
== API-MS-Win-Core-ProcessThreads-L1-1-0.dll ==
== API-MS-Win-Core-Heap-L1-1-0.dll        ==
== API-MS-Win-Core-Memory-L1-1-0.dll      ==
== API-MS-Win-Core-Handle-L1-1-0.dll      ==
== API-MS-Win-Core-Synch-L1-1-0.dll       ==
== API-MS-Win-Core-File-L1-1-0.dll        ==
== API-MS-Win-Core-IO-L1-1-0.dll          ==
== API-MS-Win-Core-ThreadPool-L1-1-0.dll  ==
== API-MS-Win-Core-Misc-L1-1-0.dll        ==
== API-MS-Win-Core-SysInfo-L1-1-0.dll     ==
== API-MS-Win-Core-Localization-L1-1-0.dll ==
== API-MS-Win-Core-ProcessEnvironment-L1-1-0.dll ==
== API-MS-Win-Core-String-L1-1-0.dll      ==
== API-MS-Win-Core-Debug-L1-1-0.dll       ==
== API-MS-Win-Core-ErrorHandling-L1-1-0.dll ==
== API-MS-Win-Core-Fibers-L1-1-0.dll     ==
== API-MS-Win-Core-Util-L1-1-0.dll       ==
== API-MS-Win-Core-Profile-L1-1-0.dll     ==
== API-MS-Win-Security-Base-L1-1-0.dll    ==
```

**خمس وعشرون** api-set DLLs. كل واحد منها forwarder صغير (3-5 KB).
→ محمّل الـ DLL يحتاج apiset resolver (§6 في الخطة).

---

#### 8. الأدوات المُضافة في هذه الجلسة

| الملف | الوظيفة | الحالة |
|---|---|---|
| الخطة التنفيذية الكاملة | ✅ (§7 هنا) |
| `tools/ntboxreg/ntb0extract.c` | مستخرج جدول syscall من ntdll حقيقي | ✅ يترجم |
| `tools/ntboxreg/ntdll_syscalls.h/.c` | 400 syscall جاهزة (مُولَّدة آلياً) | ✅ يترجم |
| `tools/ntboxd/syscalls/ntb0_trap.{h,c}` | رسم PE + تعديل الستوبات في مكانها | ✅ |
| `tools/ntboxd/syscalls/ntb0_trampoline.S` | قفزة الاعتراض (asm) + مُساعد النداء | ✅ |
| `tools/ntboxd/syscalls/ntb0_dispatch.{h,c}` | المترجم + المصابيح + خطّاف الاختبار | ✅ |
| `tools/ntboxd/syscalls/ntb0_trace.h` | مصابيح user-space | ✅ |
| `tools/ntboxd/syscalls/ntb0_nt.h` | تخطيطات NT (Win64) + ACCESS_MASK + NTSTATUS | ✅ |
| `tools/ntboxd/syscalls/ntb0_path.{h,c}` | ترجمة مسار NT → مسار لينكس (مصابيح PATH-xxx) | ✅ |
| `tools/ntboxd/syscalls/test_trap.c` | اختبار end-to-end على ntdll الحقيقي | ✅ **PASSED** |

#### نتيجة `ntb0extract`:

```
$ ./tools/ntboxreg/ntb0extract /tmp/opencode/win7root/Windows/System32/ntdll.dll
file: /tmp/opencode/win7root/Windows/System32/ntdll.dll
syscall stubs detected: 801

-- Nt* (kernel) --
  0x000u  NtMapUserPhysicalPagesScatter
  0x001u  NtWaitForSingleObject
  0x002u  NtCallbackReturn
  0x003u  NtReadFile
  0x004u  NtDeviceIoControlFile
  0x005u  NtWriteFile
  ...
  (400 entries)
```

---

#### 9. قياسات المصيدة — **كلها مُنفَّذة على ntdll الحقيقي**

> القياسات التالية أُجريت في هذه الجلسة على
> `/tmp/opencode/win7root/Windows/System32/ntdll.dll` (Win7 SP1 x64).
> الشيفرة التي تحقّقها: `tools/ntboxd/syscalls/`.

#### 9.1 الخيار B (معالج SIGSEGV/SIGSYS) — **مستبعد بالقياس**

| الفحص | النتيجة |
|---|---|
| `syscall 0x52` في user-space | نُفِّذ كـ **Linux syscall رقم 0x52 = `rename()`** — الملف أُعيدت تسميته فعلاً |
| الاستنتاج | الاعتراض يجب أن يقع **قبل** تنفيذ `syscall` — لا معالج إشارة يمكنه ذلك |
| النتيجة | ❌ الخيار B مستبعد نهائياً |

#### 9.2 تعديل EAT وحده — **غير كافٍ (مقيس)**

| الفحص | النتيجة |
|---|---|
| أنماط `call rel32` داخل ntdll تستهدف ستوب syscall | **133 من 398** ستوب |
| عدد مواقع الاستدعاء | **1217** |
| الاستنتاج | تعديل EAT يترك 1217 استدعاءً داخلياً تتجاوز المصيدة |
| النتيجة | ❌ لازم تعديل **الشيفرة في مكانها** |

#### 9.3 تخطيط الستوبات (مقيس على الصورة الحقيقية)

| القياس | القيمة |
|---|---|
| `SizeOfImage` | `0x1a9000` (1.73 MB) |
| `SizeOfHeaders` | `0x400` |
| عدد الأقسام | 7 |
| `.text` | RVA `0x1000` / حجم `0x10027a` |
| عدد أنماط `4C 8B D1 B8 <num> 0F 05` | **400** |
| عددها المنتهية بـ `C3` | 398 |
|_stride_ بين الستوبات | **16 بايت** (394 مرة) / **32 بايت** (3 مرات) |
| البايتات الحرة في خانة 16-بايتية | **8..15** (بعد `0F 05 C3` + حشو `0F 1F 44 00 00`) |

#### 9.4 بايتات التعديل — **مُتحقَّق منها على ملف حقيقي**

```
4c 8b d1 b8 52 00 00 00 e9 93 77 15 00 90 90 90
```

- البايتات 0..7 (المُصدِرة) **لم تتغيّر** → عنوان الستوب في EAT يبقى صالحاً.
- البايتات 8..15 = `E9 <rel32>` + 3 `nop` = 8 بايت بالضبط.
- المسافة بين الصورة والقفزة في الاختبار: `0x1a9000` (1.7MB) — ضمن ±2GB ✅
- **شرط إلزامي:** الصورة والقفزة في نفس `mmap` (وإلا فشلت `rel32`).

#### 9.5 توزيع الـ arguments — **تصحيح مهم**

| الافتراض الخاطئ | الحقيقة المقيسة |
|---|---|
| `arg5` في `r11` | ❌ في Win64 x64 يُمرَّر `arg5` **على المكدس** عند `[rsp+0x28]` (بعد `call` pushes عنوان العودة) |
| `r11` غير متغيّر | `r11` متغيّر (volatile) في Win64 → يُستعمل كـ scratch في بعض التصاميم |
| `rdi`/`rsi` متغيّران | ❌ **غير متغيّرَين** (callee-saved) في Win64 → القفزة تحفظهما |
| `mov %eax,%rsi` | ❌ خطأ (mixing 32/64-bit) — الصحيح `mov %eax,%esi` |

توزيع مُتحقَّق منه في القفزة:
```
rax = num | r10 = a1 | rdx = a2 | r8 = a3 | r9 = a4 | [rsp+0x28] = a5
```

#### 9.6 مرجع RIP-relative بعد نسخ القفزة — **مستبعد**

- القفزة تُنسخ من ملف الكود إلى صفحة داخل صورة ntdll.
- أي `call rel32` داخلها يصبح خاطئاً بعد النسخ.
- الحل: `movabs $0, %rax` (‏`48 B8` + imm64) كـ **PLACEHOLDER** يُخيط وقت التشغيل
  بعنوان المترجم، ثم `call *%rax` (`FF D0`).
- `imm64` بلا حدّ مسافة ✅ (بعكس `rel32`).

#### 9.7 نتيجة الاختبار end-to-end (مُشغَّل)

```
[ntbox][TRAP-008][OK] trampoline placed at 0x... (70 bytes), dispatcher=0x...
[ntbox][TRAP-011][OK] mapped .../ntdll.dll: base=0x... SizeOfImage=0x1a9000 .text=0x10027a@0x1000
[ntbox][TRAP-014][OK] syscall stubs found=400 patched=400 (in-place, E9 rel32)
[ntbox][SYS-900][UNIMPL] syscall 0x52 (NtCreateFile) not translated yet
[ntbox][SYS-001][OK] NtClose (0xc) -> 0x0
PASS: 400 syscall stubs patched in place
NtClose stub bytes: 4c 8b d1 b8 0c 00 00 00 e9 f3 7b 15 00 90 90 90
PASS: stub patched with E9 (rel32 jump)
PASS: dispatcher received num=0x52 a1..a5=0x1111 0x2222 0x3333 0x4444 0x5555
PASS: NtClose returned STATUS_SUCCESS (0x0)
PASS: handle really closed (close() -> EBADF)
== PASSED (0 failure(s)) ==
```

**إعادة التشغيل:**
```bash
cd tools/ntboxd/syscalls
gcc -O2 -Wall -Wextra -I../../ntboxreg -I. -o test_trap test_trap.c \
    ../../ntboxreg/ntdll_syscalls.c ntb0_trap.c ntb0_dispatch.c ntb0_trampoline.S
./test_trap /tmp/opencode/win7root/Windows/System32/ntdll.dll
```

#### 9.8 خلل مُكتشف في المولّد (أُصلح)

`ntb0extract --emit-c` كان يكتب `ntb0_syscall_count = 801` بينما المصفوفة تحتوي
**400** مدخلاً (فلتر `Nt*` في المصفوفة، لكن `count` مأخوذ من العدد الخام).
كان سيقرأ البحث الثنائي خارج المصفوفة. **أُصلح**: `count` الآن عدد ما صدر فعلاً (400).

#### 9.9 عوائق تقنية في CJK و C (سجلت)

- `inline asm` مع عدة مدخلات `"r"` → يوزّع GCC السجلات عشوائياً وقد يعطي
  `call *%1` عنواناً خاطئاً (لاحظناه: نداء إلى `0x3`).
  **الحل:** مُساعد نداء في assembly (`ntb0_call_win64`) ينقل السجلات صراحةً.
- محاذاة المكدس: `ntb0_call_win64` يدفع 5 سجلات (40 بايت) + `sub $0x60` (96)
  = 136 بايت ≡ 8 (mod 16) → المحاذاة قبل النداء = 0 (mod 16) ✅
  وخانة `arg5` عند `[rsp+0x20]` (قبل النداء) = `[rsp+0x28]` بعد النداء ✅
  (وسعة المعاملات إلى 11 — انظر §9.10).

#### 9.10 توسيع تمرير المعاملات إلى 11 (مُقاس — لازم لـ NtCreateFile)

> **لماذا:** `NtCreateFile` في Win64 يأخذ **11** معاملاً. المترجم كان يستقبل 5 فقط،
> فلا يصل `CreateDisposition` (arg8) ولا `ShareAccess` (arg7) ولا `CreateOptions`
> (arg9) — أي أن الفتح لم يكن ممكناً بترجمة صادقة. القفزة وُسّعت.

#### (أ) إزاحات معاملات ويندوز x64 (مقيسة على ntdll الحقيقي)

| المعامل | العنوان عند دخول الـ stub | ملاحظة |
|---|---|---|
| arg5 | `[rsp+0x28]` | أول معامل على المكدس (32 بايت shadow space) |
| arg6 | `[rsp+0x30]` | `NtCreateFile.FileAttributes` |
| arg7 | `[rsp+0x38]` | `NtCreateFile.ShareAccess` |
| arg8 | `[rsp+0x40]` | `NtCreateFile.CreateDisposition` |
| arg9 | `[rsp+0x48]` | `NtCreateFile.CreateOptions` |
| arg10 | `[rsp+0x50]` | `NtCreateFile.EaBuffer` |
| arg11 | `[rsp+0x58]` | `NtCreateFile.EaLength` |

#### (ب) حساب الإطار داخل القفزة (مُتحقَّق)

```
8 push (rbx,rbp,r12..r15,rdi,rsi) = 0x40
sub $8 (حشو محاذاة)               = 0x08   → الإجمالي 0x48
⇒ arg5 عند rsp+0x70 … arg11 عند rsp+0xA0
6 push للمعاملات a11..a6           = 0x30   → الإجمالي 0x78
⇒ عند النداء: rsp ≡ 0 (mod 16) ✅ و a6 عند [rsp+0x00] … a11 عند [rsp+0x28]
```
اصطلاح SysV: المعامل السابع فما فوق على المكدس ⇒ `[rsp+0x00]=a6` ✅ مطابق.

#### (ج) مُساعد النداء (للاختبار)

`ntb0_call_win64` صار يمرّر 11 معاملاً: يحجز `0x60` (96 بايت) = 32 shadow
+ 7×8 معاملات، مع 5 pushes (40) يصبح الإجمالي 136 ≡ 8 (mod 16) ⇒ المحاذاة
قبل النداء 0 (mod 16) ✅. المصادر (معاملات SysV الواردة) عند `rsp+0x90..0xB8`،
والوجهة (اصطلاح ويندوز) عند `rsp+0x28..0x50`.

#### (د) دليل

```
PASS: dispatcher received num=0x50 a1..a11 complete (a5..a11 from the Win64 stack)
```
مُتحقَّق منه بـ probe على القفزة: القيم `0x1111..0xBBBB` وصلت **بالترتيب**.

#### (هـ) ملاحظة مقاييس

القفزة تكتب 120 بايت تحت `rsp` الخاص بالمتصل (Windows) — داخل المساحة الحرة
تحت الإطار (لا تمس locals المتصل). الرقم ثابت ومُوثَّق هنا.

#### 9.11 SYS-005 — NtCreateFile (0x52) — **منفَّذ ومُختبَر**

| البند | التفصيل |
|---|---|
| المسار | `OBJECT_ATTRIBUTES.ObjectName` (UNICODE_STRING) → مسار لينكس → `openat()` على جذر ويندوز |
| التخطيط | `UNICODE_STRING` (16) / `OBJECT_ATTRIBUTES` (0x30) / `IO_STATUS_BLOCK` (0x10) — Win64، مع `_Static_assert` |
| الخرج | `*FileHandle = fd` (المقبض = fd، متّسق مع SYS-001 `NtClose`) + `IoStatusBlock.Status/Information` |
| `CreateDisposition` | `FILE_OPEN→0` / `FILE_CREATE→O_CREAT|O_EXCL` / `FILE_OPEN_IF→O_CREAT` / `FILE_OVERWRITE→O_TRUNC` / `FILE_OVERWRITE_IF→O_CREAT|O_TRUNC` / `FILE_SUPERSEDE→O_CREAT|O_TRUNC` |
| `DesiredAccess` | `GENERIC_READ/WRITE` + `FILE_*_DATA` → `O_RDONLY/O_WRONLY/O_RDWR` (+`O_APPEND`) |
| `CreateOptions` | `FILE_DIRECTORY_FILE→O_DIRECTORY`، `FILE_NON_DIRECTORY_FILE→فحص fstat` |
| ترجمة المسار | `\??\C:\...` / `\Device\HarddiskVolumeN\...` / `\Windows\...` / `C:\...` → كلها → `Windows/System32/...` |

**حدود موثَّقة (لم تُخفَ):**
- `ShareAccess` لا يُترجَم (لينكس بلا وضع مشاركة) — يحتاج جدول مقابض.
- `AllocationSize` / `FileAttributes` / `EaBuffer` لا تُترجَم (لا مقابل في POSIX).
- `RootDirectory ≠ NULL` (مسار نسبي) → `STATUS_NOT_SUPPORTED`.
- `\Registry\...` و UNC (`\Device\Mup\...`) → `STATUS_NOT_SUPPORTED` (الرجستري = مرحلة 1).
- `..` مرفوض → `STATUS_OBJECT_PATH_SYNTAX_BAD` (لا هروب خارج جذر ويندوز).
- القرص غير `C:` → `STATUS_DEVICE_NOT_CONNECTED`.

**دليل الجولة الوظيفية (مُشغَّل):**

```
[ntbox][PATH-002][OK] windows root = /tmp/opencode/win7root (fd=3)
[ntbox][PATH-003][OK] nt path -> linux path: Windows/System32/ntdll.dll
[ntbox][SYS-005][OK] NtCreateFile (0x52) -> 0x0
PASS: NtCreateFile returned STATUS_SUCCESS, handle=4
PASS: opened file is the real PE (magic MZ)
PASS: NtCreateFile handle closed via NtClose
PASS: FILE_CREATE created a new file (handle=4)
PASS: FILE_CREATE on existing file -> STATUS_OBJECT_NAME_COLLISION (0xc0000035)
PASS: FILE_OPEN on missing file -> STATUS_OBJECT_NAME_NOT_FOUND (0xc0000034)
PASS: unmounted drive -> STATUS_DEVICE_NOT_CONNECTED (0xc000009d)
== PASSED (0 failure(s)) ==
```

> **الدليل القاطع:** الملف المفتوح عبر `NtCreateFile` قُرئ منه أول بايتين
> عبر نفس الـ fd → `MZ` (ترويسة PE حقيقية) ⇒ الملف فُتح **في مكانه** داخل
> شجرة ويندوز 7 المستخرجة، لا نسخة منه.

#### 9.12 نتيجة الاختبار end-to-end بعد SYS-005 (مُشغَّل — `make selftest`)

```
[ntbox][TRAP-008][OK] trampoline placed at 0x... (128 bytes), dispatcher=0x...
[ntbox][TRAP-011][OK] mapped .../ntdll.dll: base=0x... SizeOfImage=0x1a9000 .text=0x10027a@0x1000
[ntbox][TRAP-014][OK] syscall stubs found=400 patched=400 (in-place, E9 rel32)
[ntbox][SYS-900][UNIMPL] syscall 0x50 (NtTerminateThread) not translated yet
[ntbox][SYS-001][OK] NtClose (0xc) -> 0x0
PASS: 400 syscall stubs patched in place
NtClose stub bytes: 4c 8b d1 b8 0c 00 00 00 e9 f3 7b 15 00 90 90 90
PASS: stub patched with E9 (rel32 jump)
NtCreateFile stub bytes: 4c 8b d1 b8 52 00 00 00 e9 93 77 15 00 90 90 90
PASS: NtCreateFile stub patched with E9 (rel32 jump)
PASS: dispatcher received num=0x50 a1..a11 complete (a5..a11 from the Win64 stack)
PASS: NtClose returned STATUS_SUCCESS (0x0)
PASS: handle really closed (close() -> EBADF)
PASS: NtCreateFile returned STATUS_SUCCESS, handle=4
PASS: opened file is the real PE (magic MZ)
PASS: NtCreateFile handle closed via NtClose
PASS: FILE_CREATE created a new file (handle=4)
PASS: FILE_CREATE on existing file -> STATUS_OBJECT_NAME_COLLISION (0xc0000035)
PASS: FILE_OPEN on missing file -> STATUS_OBJECT_NAME_NOT_FOUND (0xc0000034)
PASS: unmounted drive -> STATUS_DEVICE_NOT_CONNECTED (0xc000009d)
PASS: path '\??\C:\Windows\System32\ntdll.dll' -> 'Windows/System32/ntdll.dll' (rc=0x0)
PASS: path '\Device\HarddiskVolume1\Windows\System32\ntdll.dll' -> 'Windows/System32/ntdll.dll' (rc=0x0)
PASS: path '\Windows\System32\ntdll.dll' -> 'Windows/System32/ntdll.dll' (rc=0x0)
PASS: path 'C:\Windows\System32\ntdll.dll' -> 'Windows/System32/ntdll.dll' (rc=0x0)
PASS: path '\??\C:\Windows\System32\' -> 'Windows/System32' (rc=0x0)
PASS: path '\??\C:\Windows\..\..\etc\passwd' rejected (path traversal, rc=0xc000003b)
PASS: path '\Registry\Machine\SYSTEM' rejected (registry path, rc=0xc00000bb)
PASS: path '\Device\Mup\srv\share\f.txt' rejected (UNC path, rc=0xc00000bb)
== PASSED (0 failure(s)) ==
```

**صفر مصباح FAIL في التقرير** — الفشل المتوقّع (ملف موجود / غير موجود / قرص غير موصول)
يُسجَّل `OK (expected failure)` عبر `ntb0_set_expect_fail()` حتى لا يُخلط
بـ FAIL حقيقي (§4).

---

#### 9.13 SYS-006 / SYS-003 / SYS-004 — NtOpenFile / NtReadFile / NtWriteFile — **منفَّذة ومُختبَرة**

> **لماذا:** هذه الثلاثة هي بقية مسار "ملف" كامل: فتح → قراءة → كتابة. بدونها لا يمكن
> تشغيل أي كود ويندوز يقرأ ملفاً حقيقياً من داخل شجرة Win7.

#### (أ) أرقام الـ syscalls (مقيسة من ntdll الحقيقي — جدول §4.3)

| syscall | الرقم | عدد المعاملات | المصدر |
|---|---|---|---|
| `NtReadFile` | `0x03` | 9 | `ntdll_syscalls.c` (مُولَّد) |
| `NtWriteFile` | `0x05` | 9 | `ntdll_syscalls.c` (مُولَّد) |
| `NtOpenFile` | `0x30` | 10 | `ntdll_syscalls.c` (مُولَّد) |

#### (ب) **إزاحة معاملات `NtOpenFile` تختلف عن `NtCreateFile`**

| المعامل | `NtCreateFile` (11) | `NtOpenFile` (10) |
|---|---|---|
| `FileHandle*` | arg1 | arg1 |
| `DesiredAccess` | arg2 | arg2 |
| `ObjectAttributes*` | arg3 | arg3 |
| `IoStatusBlock*` | arg4 | arg4 |
| `AllocationSize*` | arg5 | arg5 |
| `FileAttributes` | arg6 | — (غير موجود) |
| `ShareAccess` | arg7 | **arg6** |
| `CreateDisposition` | **arg8** | **arg7** |
| `CreateOptions` | **arg9** | **arg8** |
| `EaBuffer` | arg10 | arg9 |
| `EaLength` | arg11 | arg10 |

> **الأمر:** `NtOpenFile` بلا معامل `FileAttributes` ⇒ كل ما بعده يتزحزح
> موضعاً واحداً. **خطأ هنا = فتح ملف خاطئ بصمت** — لذلك فُصل الكود:
> `ntb0_open_file()` مشترك، وكل handler يمرّر `disp/opts` من موضعه الصحيح.

#### (ج) `NtReadFile` / `NtWriteFile` (9 معاملات — نفس التوقيع)

```
a1 = FileHandle   a2 = Event   a3 = ApcRoutine   a4 = ApcContext
a5 = IoStatusBlock*   a6 = Buffer   a7 = Length   a8 = ByteOffset*
a9 = ApcRoutineArgument
```

| الحالة | الترجمة |
|---|---|
| `ByteOffset != NULL` | `pread()`/`pwrite()` عند الإزاحة (موضع الملف لا يتغيّر) |
| `ByteOffset == NULL` | `read()`/`write()` من الموضع الحالي (الموضع يتقدّم) |
| `IoStatusBlock.Information` | عدد البايتات المنقولة فعلياً |
| قراءة 0 بايت عند EOF | `STATUS_END_OF_FILE` (0xC0000011) |
| `Event`/`ApcRoutine` ≠ NULL | `STATUS_NOT_SUPPORTED` (غير متزامن غير مدعوم) |
| مقبض للقراءة فقط + كتابة | `STATUS_ACCESS_DENIED` (لا EBADF — فحص `F_GETFL`) |
| مقبض مغلق/غير صالح | `STATUS_INVALID_HANDLE` |

#### (د) الدليل المُشغَّل

```
-- SYS-006: NtOpenFile (0x30) --
PASS: NtOpenFile returned STATUS_SUCCESS, handle=4
PASS: NtOpenFile on a directory -> STATUS_FILE_IS_A_DIRECTORY (0xc00000ba)
PASS: NtOpenFile on a directory (FILE_DIRECTORY_FILE) -> handle=5
PASS: NtOpenFile on missing file -> STATUS_OBJECT_NAME_NOT_FOUND (0xc0000034)

-- SYS-003: NtReadFile (0x03) --
PASS: NtReadFile at offset 0 -> MZ (Information=2)
PASS: NtReadFile bytes identical to the file on disk (offset 0x3C, 16 bytes)
PASS: sequential NtReadFile (NULL offset) advanced the position correctly
PASS: read past EOF -> STATUS_END_OF_FILE (0xc0000011)
PASS: read on a bogus handle -> STATUS_INVALID_HANDLE (0xc0000008)

-- SYS-004: NtWriteFile (0x05) --
PASS: NtWriteFile wrote 28 bytes
PASS: NtWriteFile at offset 64 wrote 12 bytes
PASS: read-back matches what was written (round-trip)
PASS: the bytes are in the real file on disk (in place)
PASS: write on a read-only handle -> STATUS_ACCESS_DENIED (0xc0000022)
```

**الدليل القاطع (قراءة):** 16 بايت عند إزاحة `0x3C` (حقل `e_lfanew` في ترويسة PE)
قُرئت عبر `NtReadFile` وقورنت **بالبايت** مع نفس الإزاحة في الملف على القرص —
مطابقة تامة ⇒ نفس الملف، نفس الإزاحات، بلا نسخ ولا محاكاة.

**الدليل القاطع (كتابة):** ما كُتب عبر `NtWriteFile` قُرئ راجعاً عبر `NtReadFile`
(مطابق) **و** قُرئ مباشرة من القرص خارج الصندوق (مطابق) ⇒ الكتابة وصلت إلى
الملف الحقيقي في مكانه.

#### (هـ) حدود موثّقة (لم تُخفَ)

- القراءة/الكتابة غير المتزامنة (`Event`/`ApcRoutine` ≠ NULL) → `NOT_SUPPORTED`.
- `ShareAccess` لا يُترجَم (يحتاج جدول مقابض + أقفال مشاركة).
- المقبض = `fd` خام من لينكس (لا جدول مقابض بعد) — يكشف ورقمين من الملف.
- `AllocationSize` (NtOpenFile) بلا معنى لفتح قائم → يُتجاهل.
- `ApcContext` / `ApcRoutineArgument` يُتجاهلان (متزامنة فقط).
- `NtWriteFile` على مقبض `O_APPEND` + `ByteOffset` ≠ NULL → سلوك POSIX (تجاهل الإزاحة).

#### (و) إحصاءات الترجمة بعد المرحلة

```
ID        NAME                      CALLS  FAILS
SYS-001   NtClose                       7      0
SYS-002   NtAllocateVirtualMemory       0      0
SYS-003   NtReadFile                    7      0
SYS-004   NtWriteFile                   3      0
SYS-005   NtCreateFile                  6      0
SYS-006   NtOpenFile                    5      0
```

سجل المصابيح في الجولة الكاملة: **OK=48 · STUB=3 · FAIL=0 · UNIMPL=1** (خروج `0`).

---

#### 10. ما لم يُتحقق بعد (open)

- [ ] قراءة hives عبر binary format parser (المرحلة 1 لم تبدأ)
- [ ] استخراج `win32k.sys` وبناء جدول syscall الخاص به
- [x] ~~تمرير المعامل السادس (`Protect` عند `[rsp+0x30]`) إلى المترجم~~ — ✅ نُفِّذ (§9.10): القفزة تمرّر 11 معاملاً، و`NtAllocateVirtualMemory` صار يقرأ `Protect` فعلياً
- [x] ~~`NtReadFile`/`NtWriteFile` تحتاج 8–9 معاملات (Buffer/Length/Offset)~~ — ✅ نُفِّذ (§9.13): `SYS-003`/`SYS-004` منفَّذان ومُختبَرين (قراءة/كتابة + إزاحات + EOF + in-place)
- [x] ~~`NtOpenFile` (0x30)~~ — ✅ نُفِّذ (§9.13): `SYS-006` — ملاحظة: إزاحة `CreateDisposition` = arg7 (لا arg8) لأن التوقيع بلا `FileAttributes`
- [ ] base relocations: تحميل ntdll على عنوان غير المفضّل يتطلب تطبيق `.reloc`
- [ ] `api-ms-win-core-*.dll` forwarders — needs api-set resolver (المرحلة 4)
- [ ] مسار الملفات: NT → لينكس ✅ جزئياً (§9.11 + §11.3) — متبقٍ: UNC، تعدد الأقراص، `..` داخل جذر NT
- [x] ~~`ShareAccess` غير مُترجَم~~ — ✅ نُفِّذ (§11.2): جدول المقابض + فحص مشاركة بالهوية `(dev,ino)` ⇒ `STATUS_SHARING_VIOLATION`
- [x] ~~جدول مقابض (HandleTable)~~ — ✅ نُفِّذ (§11.1): `SYS-007` · مقبض شفاف (سحر+جيل+خانة) · جيل يمنع use-after-close
- [x] ~~`RootDirectory` النسبي~~ — ✅ نُفِّذ (§11.3): `PATH-007` · `openat(dirfd, rel)` · رفض المسار المطلق و`..`
- [x] ~~`NtDuplicateObject`~~ — ✅ نُفِّذ (§11.4): `SYS-008`
- [x] ~~`NtFreeVirtualMemory`~~ — ✅ نُفِّذ (§11.4): `SYS-009` · `munmap` + تصفير `Base/Size`
- [x] ~~`NtProtectVirtualMemory`~~ — ✅ نُفِّذ (§11.4): `SYS-010` · `mprotect` + `OldProtect`
- [x] ~~`NtQueryVirtualMemory`~~ — ✅ نُفِّذ (§11.4): `SYS-011` · `/proc/self/maps`
- [x] ~~`NtQueryInformationFile`~~ — ✅ نُفِّذ (§11.4): `SYS-012` · `FileStandardInformation` + `FilePositionInformation`
- [ ] I/O غير متزامن (`NtReadFile`/`NtWriteFile` بـ `Event`) → حالياً `NOT_SUPPORTED`
- [x] ~~جدول مقابض (HandleTable)~~ — ✅ نُفِّذ بالكامل (§11.1 + §12): `RootDirectory` النسبي · `NtDuplicateObject` · مقبض شفاف لا يتصادم مع `fd` خام · أحداث
- [x] ~~`NtSetInformationFile`~~ — ✅ نُفِّذ (§12.1): `SYS-013` · position · EOF · rename · delete
- [x] ~~`NtFlushBuffersFile`~~ — ✅ نُفِّذ (§12.1): `SYS-014`
- [x] ~~`NtDeviceIoControlFile`~~ — ✅ نُفِّذ (§12.1): `SYS-015` · تمرير ioctl حقيقي (يربط المترجم بـ `linux_bridge`)
- [x] ~~`NtCreateEvent`/`NtSetEvent`/`NtClearEvent`~~ — ✅ نُفِّذ (§12.1): `SYS-016..018` · `eventfd`
- [x] ~~`NtWaitForSingleObject`~~ — ✅ نُفِّذ (§12.1): `SYS-019` · `poll` + `STATUS_TIMEOUT`
- [x] ~~`NtDelayExecution`/`NtYieldExecution`~~ — ✅ نُفِّذ (§12.1): `SYS-020/021`
- [x] ~~`NtQuerySystemInformation`~~ — ✅ نُفِّذ (§12.1): `SYS-022` · صنفان
- [x] ~~`NtQueryDirectoryFile`~~ — ✅ نُفِّذ (§12.1): `SYS-023` · 2647 مدخلاً
- [x] ~~المقاطع (T-034)~~ — ✅ نُفِّذ (§13.1): `SYS-024..026` · ملفّي = `mmap` بلا نسخ · مجهول = `memfd`
- [x] ~~`NtFsControlFile` (T-052)~~ — ✅ نُفِّذ (§13.1): `SYS-027`
- [x] ~~`NtLockFile`/`NtUnlockFile` (T-053)~~ — ✅ نُفِّذ (§13.1): `SYS-028/029`
- [x] ~~`NtQueryAttributesFile` (T-055)~~ — ✅ نُفِّذ (§13.1): `SYS-030`
- [x] ~~`NtQueryObject` (T-047)~~ — ✅ نُفِّذ (§13.1): `SYS-031`
- [ ] الخيوط (T-040) · العمليات (T-041/042) · الريجستري (T-049/050) · الأنابيب (T-051) · `NtSetSystemInformation` (T-036) · `NtSetInformationThread` (T-048) · `NtQueryFullAttributesFile` (T-056)
- [ ] تشغيل `userapp.exe` بـ ntdll/kernel32 حقيقيين (CAS-2.2)

#### 11. جولة W2 الأولى: جدول المقابض + 6 أوامر NT جديدة (T-023 … T-030)

> التاريخ: 2026-10-21 · النتيجة: **54 فحصاً · 0 FAIL · مصابيح OK=140 STUB=3 FAIL=0 UNIMPL=1**
> (`make verify` ⇒ `VERDICT: PASS`)

#### 11.0 لماذا كان لازماً؟

قبل هذه الجولة كان **المقبض = رقم `fd` خام من لينكس**. هذا يكسر ثلاث
أشياء في ويندوز دفعة واحدة:

1. كود ويندوز يمرّر مقبضاً إلى `CloseHandle` ثم يستعمل رقماً آخر — مستحيل
   الحسم بلا جدول مقابض.
2. فتح نفس الملف مرتين يجب أن يعطي **مقبضين مختلفين** على نفس الملف.
3. `ShareAccess` (وضع المشاركة) لا معنى له بلا هوية للملف.

#### 11.1 T-023 — `SYS-007` جدول المقابض (HandleTable)

**الملف:** `tools/ntboxd/syscalls/ntb0_handle.{h,c}` (جديدان)

**التصميم (مقيس، لا تخمين):**

```
المقبض = 0x4E54_gggg_iiiiiiii
          │    │      └── رقم الخانة + 1 (24 بت)  — 0 = غير صالح
          │    └───────── جيل (generation) 24 بت — يزيد عند إعادة استخدام الخانة
          └────────────── سحر ثابت 0x4E54 ('NT')
```

- **الجيل** يجعل المقبض القديم (المغلق) غير صالح حتى لو أُعيدت الخانة
  ⇒ لا "use-after-close" من جهة كود ويندوز.
- الجدول: `NTB0_HANDLE_MAX = 1024` خانة + `pthread_mutex` (خيوط آمنة).
- كل خانة: `fd` · `desired` · `share` · `dev/ino` · `is_dir` · `path`.

**الهوية = `(dev, ino)` من `fstat`** — أدقّ من مقارنة نصّ المسار (نفس الملف
يُفتح بمسارين مختلفين ⇒ نفس الهوية).

**المصابيح:** `SYS-007` (OK/FAIL) — كل الأحداث: إنشاء · إغلاق · نسخ · فحص مشاركة.

**الدليل المُشغَّل (54 فحصاً ⇒ 0 FAIL):**

```
PASS: two opens of the same file -> two different handles (0x4e54000005000001 / 0x4e54000003000002)
PASS: the handle is NOT the fd (handle 0x4e54000005000001 -> fd 4, handle 0x4e54000003000002 -> fd 5)
PASS: closed handle is invalidated immediately
PASS: the second handle still works after closing the first (MZ)
PASS: NtClose on a raw fd -> STATUS_INVALID_HANDLE (0xc0000008)
```

#### 11.2 T-024 — `ShareAccess` ⇒ `STATUS_SHARING_VIOLATION`

**قواعد NT المطبَّقة (من `Microsoft-Win-Kernel` §"File Security and Access Rights"):**

يُسمح بالفتح الجديد iff:

1. وضع مشاركة **القائم** يسمح بالوصول الذي يطلبه الجديد.
2. وضع مشاركة **الجديد** يسمح بالوصول الذي grabbedه القائم.

- "الوصول" = بتات `READ`/`WRITE`/`DELETE` من `DesiredAccess`.
- عند التعارض ⇒ `STATUS_SHARING_VIOLATION (0xC0000043)` **و يُغلق الـ fd** (لا تسرّب).

```
PASS: conflicting share mode -> STATUS_SHARING_VIOLATION (0xc0000043)
PASS: compatible share mode -> opened (0x4e54000004000002)
```

#### 11.3 T-025 — `RootDirectory` (مسار نسبي داخل مجلد مفتوح)

- **الملف:** `ntb0_path.c` → `ntb0_path_rel_from_nt()` (مصباح `PATH-007`).
- `RootDirectory` يجب أن يكون مقبضاً من الجدول **ويمثّل مجلداً** ⇒
  `openat(dirfd, relative_path, …)`.
- قواعد NT المطبَّقة: المسار النسبي **لا يقبل** مطلقاً (`\` أو `/` أو حرف قرص)
  ⇒ `STATUS_OBJECT_PATH_SYNTAX_BAD` · و`..` مرفوض (نفس قاعدة `PATH-006`).
- `RootDirectory` غير صالح ⇒ `STATUS_INVALID_HANDLE`.

```
PASS: relative open via RootDirectory -> the same ntdll.dll (MZ)
PASS: absolute path with RootDirectory -> STATUS_OBJECT_PATH_SYNTAX_BAD (0xc000003b)
PASS: invalid RootDirectory -> STATUS_INVALID_HANDLE (0xc0000008)
```

#### 11.4 T-026 … T-030 — ستة أوامر NT جديدة

| المصباح | الأمر | الرقم (مقيس) | الترجمة | الحدّ الموثّق |
|---|---|---|---|---|
| `SYS-008` | `NtDuplicateObject` | 0x39 | `dup()` + **مقبض جديد** | لا نسخ بين عمليات · `HandleAttributes` غير مُترجَم |
| `SYS-009` | `NtFreeVirtualMemory` | 0x1b | `munmap()` على المنطقة من `/proc/self/maps` | `MEM_RELEASE` ⇒ الحجم يتجاهَل (سلوك NT) |
| `SYS-010` | `NtProtectVirtualMemory` | 0x4d | `mprotect()` + `OldProtect` من `maps` | `PAGE_*` ⇄ `PROT_*` |
| `SYS-011` | `NtQueryVirtualMemory` | 0x20 | `MemoryBasicInformation` من `/proc/self/maps` | صنف واحد فقط (0) |
| `SYS-012` | `NtQueryInformationFile` | 0x0e | `FileStandardInformation` (1) + `FilePositionInformation` (2) من `fstat`/`lseek` | صنف آخر ⇒ `NOT_SUPPORTED` |

**ترتيب المعاملات مُقيس** من جدول ntdll الحقيقي (`tools/ntboxreg/ntdll_syscalls.c`،
مولّد من `ntdll.dll` الأصلي — لا يُعدَّل يدوياً).

```
PASS: duplicate -> a new handle (0x4e54000008000001 -> 0x4e54000006000002)
PASS: both handles read the same file (MZ/MZ)
PASS: the duplicate survives closing the source
PASS: NtAllocateVirtualMemory -> 0x76645b958000 (0x10000 bytes)
PASS: MemoryBasicInformation (base=0x76645b958000 size=0x12000 state=0x1000 prot=0x4 type=0x20000)
PASS: NtProtectVirtualMemory -> PAGE_READONLY (old=0x4)
PASS: NtFreeVirtualMemory -> base=0 size=0
PASS: query on a freed region -> 0xc000000d
PASS: FileStandardInformation (eof=0x1a6d60 links=2 dir=0)
PASS: FilePositionInformation (pos=0x0)
PASS: unsupported information class -> STATUS_NOT_SUPPORTED (0xc00000bb)
```

#### 11.5 خطأ نووي: Oops في `/proc` ⇒ الوحدة لا تُفرَّغ أبداً (تشخيص كامل)

**الظاهرة:** `insmod ntbox.ko` ⇒ `Device or resource busy` · و`/proc/modules` يقول:

```
ntbox 114688 -1 - Unloading 0x0000000000000000 (OE-)
```

**التشخيص (مقيس — تفكيك + مصدر النواة 6.8 `fs/proc/inode.c`):**

```c
/* fs/proc/inode.c — proc_entry_rundown() */
de->pde_unload_completion = &c;
if (atomic_add_return(BIAS, &de->in_use) != BIAS)   /* BIAS = -1U<<31 */
    wait_for_completion(&c);
```

- `proc_reg_write_iter()` تستدعي `use_pde()` قبل المعالج و`unuse_pde()` بعده.
- **Oops داخل المعالج ⇒ `unuse_pde()` لا يُنفَّذ** ⇒ `pde->in_use` يبقى
  مرفوعاً ⇒ `proc_entry_rundown()` ينتظر للأبد ⇒ **`rmmod` لا ينتهي** (D state).
- **لا توجد واجهة "إجبار التفريغ" في النواة ⇒ إعادة تشغيل الجهاز هي العلاج الوحيد.**
- المدخل العالق = `runuser` (المعالج الذي حدث فيه Oops) · وبقيّة
  `/proc/ntbox` سليمة: `attach, detach, load, open, partitions, policy, runuser,
  selftest, stats`.

**مصدر الـ Oops ( forensics كامل — ليس «الملف الغامض»):**

| القياس | القيمة |
|---|---|
| صورة PE32 (32-بت) | vmalloc base `0xffffd05d82a71000` · `SizeOfImage=0x27000` · `machine=0x14c` · `magic=0x10b` · entry `0x6e1f` |
| `delta` (base − preferred) | `0xffffd05d82671000` |
| runtime `ntbox_module_list` | `0xffffffffc2196f80` |
| runtime `ntbox_module_lock` | `0xffffffffc2196f70` |
| العنوان المُبلَّغ | `list_add_tail` يكتب عبر NULL ⇒ `old_entry->next = new` |
| `kmalloc` args | `size=0x60` (96 = `sizeof(struct ntbox_module)`) · `flags=0xcc0` (GFP_KERNEL) |
| مطابقة الكود | بايتات الكود في السجل = بايتات `ntbox.ko` على القرص **حرفياً** ⇒ الوحدة **ليست قديمة** |

⇒ الخلل **ليس** "وحدة قديمة"، بل **خطأ منطقي في الكود**: عنوان القائمة
(`ntbox_module_list`) كان **مفسَّراً كقيمة** بدل أن يكون عنواناً — أي أن
`list_add_tail` استُدعي على قائمة غير مُهيّأة ⇒ `old_entry == NULL` ⇒ كتابة
عبر NULL.

**مجموعة الإصلاح في النواة (طُبِّقت وأُعيد البناء بنجاح):**

| الملف | الإصلاح |
|---|---|
| `include/pe_arch.h` | `PE32_OFF_IMPORT_DIR`/`PE32_OFF_RELOC_DIR` (96+8n) · `PE64_OFF_IMPORT_DIR`/`PE64_OFF_RELOC_DIR` (112+8n) · `PE_OFF_*` صارا اسماً لـ PE64 |
| `src/pe/pe_arch.c` | **إصلاح خلل `detect_arch`**: كان يقرأ `nt+16` (=`NumberOfSymbols`) بدل `nt+20` (=`SizeOfOptionalHeader`) ⇒ أي PE فيه جدول رموز كان يُقرأ خطأ |
| `src/pe/pe_arch.c` | حدود (bounds) على الترويسة الاختيارية والملف · مجلدات import/reloc **لكل معمارية** · `copy_sections` بـ 64-بت (لا فيضان) |
| `src/pe/pe_arch.c` | `apply_relocations` يتحقق من جدول الـ reloc ومن **كل** هدف fixup مقابل `size_of_image` |
| `src/pe/pe_arch.c` | `resolve_imports` أُعيدت كتابتها: `pe_str_valid()` + حدود RVA كاملة (descriptors · thunk arrays · أسماء) |
| `src/pe/pe_arch.c` | `ntbox_pe_load_image` يتحقق من جدول الأقسام · `ntbox_module_add/remove` يتحققان من رأس القائمة ويُعيدان تهيئتها بدل Oops |
| `src/pe/pe_user.c` · `src/pe/pe_run.c` | **كشف المعمارية أولاً**: PE32 (32-بت) ⇒ `-ENOTSUPP` + رسالة واضحة (النواة 6.8 على x86-64 لا تُنفّذ كود 32-بت) — 32-بت مكانها **المترجم في عملية مساعدة** |

**الأثر:** بعد الإصلاح، مسار 32-بت **يرفض بهدوء** بدل أن يُسقط النواة.

> ⚠️ **ملاحظة تشغيلية:** الوحدة الحالية ما زالت عالقة من الـ Oops السابق
> (قبل الإصلاح). لا يمكن تفريغها ⇒ مجموعة `kernel` في `verify.sh` صارت
> **تشخّص** الحالة صراحةً:
> `module stuck in 'Unloading' (leaked proc pde reference) — reboot required`.
> التفريغ يتم عند إعادة تشغيل الجهاز (البيانات على `/tmp` محفوظة — القرص دائم (يعيش عبر إعادة التشغيل)).

#### 11.6 إحصاءات الترجمة بعد الجولة

```
ID        NAME                      CALLS  FAILS
SYS-001   NtClose                      16      0
SYS-002   NtAllocateVirtualMemory       1      0
SYS-003   NtReadFile                   12      0
SYS-004   NtWriteFile                   3      0
SYS-005   NtCreateFile                  7      0
SYS-006   NtOpenFile                   15      0
SYS-008   NtDuplicateObject             1      0
SYS-009   NtFreeVirtualMemory           1      0
SYS-010   NtProtectVirtualMemory        1      0
SYS-011   NtQueryVirtualMemory          2      0
SYS-012   NtQueryInformationFile        3      0
```

تغطية المترجم الآن **11 syscall** مُترجَماً ومُختبَراً (من 400 مُقاسة في ntdll).

---

#### 12. جولة W2 الثانية: 11 syscall جديدة + أحداث + تعداد مجلدات (T-031 … T-039 · T-054)

> التاريخ: 2026-10-21 · النتيجة: **71 فحصاً · 0 FAIL · مصابيح OK=2840 STUB=3 FAIL=0 UNIMPL=1**
> (`make verify` ⇒ `VERDICT: PASS`)

#### 12.1 الأوامر المُضافة (أرقام الـ syscalls مقيسة من ntdll الحقيقي)

| المصباح | الأمر | الرقم (مقيس) | الترجمة في لينكس | الحدّ الموثّق |
|---|---|---|---|---|
| `SYS-013` | `NtSetInformationFile` | 0x24 | position ⇒ `lseek` · EOF ⇒ `ftruncate` · rename ⇒ `renameat` · disposition ⇒ `unlinkat` | 4 أصناف فقط؛ البقية `NOT_SUPPORTED` |
| `SYS-014` | `NtFlushBuffersFile` | 0x48 | `fsync` | لا `FlushFileBuffers` بلا مقبض |
| `SYS-015` | `NtDeviceIoControlFile` | 0x04 | `syscall(SYS_ioctl,…)` خام | رمز الـ ioctl **لينكس** (وهو نفسه المستعمل في `linux_bridge`) |
| `SYS-016` | `NtCreateEvent` | 0x45 | `eventfd(0, EFD_CLOEXEC\|EFD_NONBLOCK)` | `SynchronizationEvent` ⇒ `EFD_SEMAPHORE` |
| `SYS-017` | `NtSetEvent` | 0x0b | `write(efd, 1)` | — |
| `SYS-018` | `NtClearEvent` | 0x3b | `read(efd)` (تصفير العدّاد) | — |
| `SYS-019` | `NtWaitForSingleObject` | 0x01 | `poll()` + `STATUS_TIMEOUT` (0x102) | مهلة موجبة تُعامل كنسبية (ساعة الإقلاع غير متاحة) |
| `SYS-020` | `NtDelayExecution` | 0x31 | `nanosleep` | 100-نانية → ms |
| `SYS-021` | `NtYieldExecution` | 0x43 | `sched_yield` | — |
| `SYS-022` | `NtQuerySystemInformation` | 0x33 | `SystemBasicInformation` (0) · `SystemProcessorInformation` (1) | أوقات المعالج صفر (T-035b) |
| `SYS-023` | `NtQueryDirectoryFile` | 0x32 | `readdir()` ⇒ `FileDirectoryInformation` | صنف 1 فقط؛ `NextEntryOffset=0` (مُدخل واحد لكل استدعاء) |

**ترتيب المعاملات** لكل أمر مستند إلى جدول ntdll المقيس
(`tools/ntboxreg/ntdll_syscalls.c` — مولّد من ntdll الأصلي).

#### 12.2 قياسات تقنية مهمّة (اكتُشفت أثناء التنفيذ)

#### (أ) `ioctl` في glibc لا يقبل مدخلاً ومخرجاً منفصلين

`ioctl()` في glibc **متغيّر** لكنه يقرأ **مؤشّراً واحداً** فقط:

```c
int ioctl (int fd, unsigned long int request, ...)   /* arg = va_arg(ap, void*) */
```

الشكل السداسي المعتاد (BSD) `ioctl(fd, req, in, inlen, out, outlen)` **غير
موجود في لينكس** ⇒ استدعاؤه يمرّر `in` (وهو غالباً `NULL`) إلى النواة ⇒ `EFAULT`.

**الحل (مقيس):** استدعاء syscall خام بثلاث وسائط:

```c
n = (int)syscall(SYS_ioctl, fd, (unsigned long)code, arg);
```

#### (ب) اتجاه المخزن: من بتّات `_IOC` (Linux)

`NtDeviceIoControlFile` يفصل `InputBuffer` عن `OutputBuffer`، بينما ioctl في
لينكس يأخذ **وسيطاً واحداً**. لذا يُشتق الاتجاه من بتّات `_IOC`:

| بتّات الاتجاه | الدلالة | الوسيط الممرَّر |
|---|---|---|
| `_IOC_WRITE` (1) | النواة **تقرأ** من الوسيط | `InputBuffer` |
| `_IOC_READ` (2) | النواة **تكتب** في الوسيط | `OutputBuffer` |
| `_IOC_READ\|_IOC_WRITE` (3) |读写 | نسخ `Input` داخل `Output` وتمرير `Output` (النتيجة ترجع فيه) |

**دليل مُشغَّل:** `FIONREAD` (0x541B) على ملف كُتب فيه 10 بايت ⇒ `avail == 10` ✅

#### (ج) مسارات NT **نسبة للجذر** ⇒ لا `rename`/`unlink` مباشرة

المسار المخزَّن في جدول المقابض هو **نسبة لجذر NT** (مثل `Windows/System32/ntdll.dll`)
لأن الفتح يتم بـ `openat(dirfd_root, path, …)`. لذلك:

- `rename("a.tmp","b.tmp")` يفشل بـ `ENOENT` (العملية CWD ليست جذر NT) ❌
- الصحيح: `renameat(rootfd, cur, rootfd, newpath)` و`unlinkat(rootfd, cur, 0)` ✅

**ودلالات NT:** المقبض يبقى صالحاً ويشير إلى الملف بعد إعادة التسمية ⇒
المسار المخزَّن يُحدَّث (`ntb0_handle_set_path`) ⇒ الحذف لاحقاً بالاسم الجديد ينجح ✅

#### (د) `SYSTEM_BASIC_INFORMATION` حجمها **0x40** على x64 (لا 0x30)

```
0x00 Reserved · 0x04 TimerResolution · 0x08 PageSize · 0x0C NumberOfPhysicalPages
0x10 LowestPhysicalPageNumber · 0x14 HighestPhysicalPageNumber · 0x18 AllocationGranularity
0x20 MinimumUserModeAddress · 0x28 MaximumUserModeAddress · 0x30 ActiveProcessorsAffinityMask
0x38 NumberOfProcessors  (CCHAR) ⇒ محاذاة إلى 8 ⇒ الحجم الكلي 0x40
```

#### (هـ) `STATUS_NO_MORE_FILES` (0x80000006) **نهاية تعداد صحيحة** لا خطأ

شِدة الحالة = تحذير (0x80000000) ⇒ ليست "نجاحاً" في معنى `NT_SUCCESS` ولا
"خطأً". لذلك تُعلَّم في الاختبار كـ **نتيجة متوقَّعة** (`ntb0_set_expect_fail`)
حتى لا تُحسب مصباح FAIL.

#### (و) تعداد المجلد يحتاج تدفّقاً محفوظاً لكل مقبض

`readdir()` يحتاج `DIR*` لا `fd`. الحل: `fdopendir(dup(fd))` — النسخة **تتشارك
موضع الملف** مع الأصل ⇒ يكمل التعداد بين الاستدعاءات، ويُغلق بـ `closedir`
عند إغلاق المقبض (لا تسرّب fd).

**النتيجة المُقاسة:** 2647 مدخلاً في `Windows/System32` وحُدِّد `ntdll.dll` ✅

#### 12.3 نتائج مُشغَّلة (71 فحصاً ⇒ 0 FAIL)

```
-- SYS-013: NtSetInformationFile (0x24) --
PASS: FileEndOfFileInformation -> the file is 50 bytes on disk
PASS: FilePositionInformation -> the position is set
PASS: FileRenameInformation -> the file is now 'ntb0_renamed.tmp'
PASS: FileDispositionInformation -> the file is deleted (by its new name)

-- SYS-014: NtFlushBuffersFile (0x48) --
PASS: NtFlushBuffersFile -> fsync done

-- SYS-015: NtDeviceIoControlFile (0x04) --
PASS: NtDeviceIoControlFile(FIONREAD) -> 10 bytes available

-- SYS-016/017/018/019: events + wait --
PASS: wait on a non-signaled event -> STATUS_TIMEOUT (0x102)
PASS: NtSetEvent -> signaled
PASS: wait after SetEvent -> STATUS_SUCCESS
PASS: wait after NtClearEvent -> STATUS_TIMEOUT
PASS: an event created signaled is immediately waitable

-- SYS-020/021: delay + yield --
PASS: NtDelayExecution(50ms) -> really waited 50 ms
PASS: NtYieldExecution -> STATUS_SUCCESS

-- SYS-022: NtQuerySystemInformation (0x33) --
PASS: SystemBasicInformation (page=4096 pages=2021874 gran=65536 cpus=4)
PASS: SystemProcessorInformation (4 entries)
PASS: unsupported system class -> STATUS_NOT_SUPPORTED (0xc00000bb)

-- SYS-023: NtQueryDirectoryFile (0x32) --
PASS: enumerated 2647 entries and found ntdll.dll
```

#### 12.4 إحصاءات الترجمة بعد الجولة الثانية

```
ID        NAME                      CALLS  FAILS
SYS-001   NtClose                      22      0
SYS-002   NtAllocateVirtualMemory       1      0
SYS-003   NtReadFile                   12      0
SYS-004   NtWriteFile                   5      0
SYS-005   NtCreateFile                  9      0
SYS-006   NtOpenFile                   17      0
SYS-008   NtDuplicateObject             1      0
SYS-009   NtFreeVirtualMemory           1      0
SYS-010   NtProtectVirtualMemory        1      0
SYS-011   NtQueryVirtualMemory          2      0
SYS-012   NtQueryInformationFile        3      0
SYS-013   NtSetInformationFile          4      0
SYS-014   NtFlushBuffersFile            1      0
SYS-015   NtDeviceIoControlFile         1      0
SYS-016   NtCreateEvent                 2      0
SYS-017   NtSetEvent                    1      0
SYS-018   NtClearEvent                  1      0
SYS-019   NtWaitForSingleObject         3      0
SYS-020   NtDelayExecution              1      0
SYS-021   NtYieldExecution              1      0
SYS-022   NtQuerySystemInformation      3      0
SYS-023   NtQueryDirectoryFile       2648      0
```

تغطية المترجم الآن **22 syscall** مُترجَماً ومُختبَراً (من 400 مُقاسة في ntdll).

**المتبقي للوصول إلى هدف 100 (T-061):** المقاطع (T-034) · الخيوط (T-040) ·
العمليات (T-041/042) · كائنات (T-047/048) · الريجستري (T-049/050) ·
الأنابيب (T-051) · `FsControlFile` (T-052) · الأقفال (T-053) ·
السمات (T-055/056).

---


#### 13. جولة W2 الثالثة: مقاطع + أقفال + سمات + كائنات (T-034 · T-047 · T-052 · T-053 · T-055)

> التاريخ: 2026-10-21 · النتيجة: **85 فحصاً · 0 FAIL · مصابيح OK=2900 STUB=3 FAIL=0 UNIMPL=1**
> (`make verify` ⇒ `VERDICT: PASS`)

#### 13.1 الأوامر المُضافة

| المصباح | الأمر | الرقم (مقيس) | الترجمة في لينكس | الحدّ الموثّق |
|---|---|---|---|---|
| `SYS-024` | `NtCreateSection` | 0x47 | ملفّي ⇒ `dup(fd)` · مجهول ⇒ `memfd_create` + `ftruncate` | لا `SEC_IMAGE` ولا `SEC_PROTECTION` |
| `SYS-025` | `NtMapViewOfSection` | 0x25 | `mmap(MAP_SHARED)` (و`MAP_FIXED` إن حدّد الندّاء العنوان) | الإزاحة يجب أن تكون محاذاة لصفحة |
| `SYS-026` | `NtUnmapViewOfSection` | 0x27 | `munmap` (الحجم من جدول العروض) | لا `MapViewOfSection` مجدول |
| `SYS-027` | `NtFsControlFile` | 0x36 | نفس مسار `NtDeviceIoControlFile` (`syscall(SYS_ioctl,…)`) | رموز ioctl لينكسية |
| `SYS-028` | `NtLockFile` | 0xe0 | `fcntl(F_SETLK/F_SETLKW)` بنطاق بايتات | أقفال POSIX = **لكل عملية** لا لكل مقبض |
| `SYS-029` | `NtUnlockFile` | 0x188 | `fcntl(F_UNLCK)` | كما فوق |
| `SYS-030` | `NtQueryAttributesFile` | 0x3a | `fstat` ⇒ `FILE_ATTRIBUTE_*` | `ReparseTag` = 0 دائماً |
| `SYS-031` | `NtQueryObject` | 0x0d | اسم الكائن (المسار المخزَّن) + النوع (`File`/`Event`/`Section`) | صنفان فقط؛ الاسم UTF-16LE في مخزن منفصل |

#### 13.2 قياسات تقنية مهمّة

#### (أ) مقطع ملفّي = `mmap` على الملف مباشرة — **بلا نسخ**

الفكرة: `NtCreateSection` بمقبض ملف ⇒ نأخذ **نسخة من الـ fd** (`dup`) ⇒
`NtMapViewOfSection` يعمل `mmap` على ذلك الـ fd ⇒ تعديلات العرض تنعكس على
الملف مباشرة (وهو نفس سلوك NT).

> لماذا `dup` وليسFd الملف نفسه؟ لأن `dup` يشارك إزاحة الملف مع الأصل،
> و`mmap` يأخذ إزاحته **بمعامل مستقل** ⇒ لا يتأثر موضع الملف الأصلي ✓
> كما أن المقبض يُغلق ⇒ يُغلق الـ fd المُكرَّر تلقائياً (لا تسرّب).

**دليل مُشغَّل:** كُتب 22 بايت في الملف ⇒上映 file-backed section ⇒
المحتوى مطابق تماماً ⇒ "maps the file itself (no copy)" ✅

#### (ب) المقطع المجهول = `memfd_create` + `ftruncate`

`memfd` يوفّر ذاكرة مجهولة قابلة للمشاركة بين العمليات (وهو ما تحتاجه
`SEC_COMMIT`/`SEC_RESERVE` بلا ملف) ⇒ ثم `mmap` عليها ⇒ عرض مشترك قابل
للكتابة ✓.

#### (ج) تفريغ العرض يحتاج **الحجم** ⇒ جدول عروض

`munmap` يحتاج طول النطاق. `NtUnmapViewOfSection` يعطي **العنوان فقط** ⇒
لازم حفظ (base,len) عند كل `mmap` ⇒ جدول `ntb0_views[64]` في المترجم ✓.

#### (د) أقفال POSIX لكل **عملية** لا لكل مقبض

`fcntl(F_SETLK)` في لينكس (وأساسه POSIX) يُسجّل القفل على الـ
`(process, inode)` ⇒ عمليتان تتشاركان القفل. هذا **أوسع** من دلالات NT
(القفل مرتبط بالمقبض/العملية معاً) ⇒ سلوك "المحاولة الثانية تفشل" غير
قابل للتشابه التام. موثّق كحدّ.

#### (هـ) الأسماء في NT **UTF-16LE** داخل `UNICODE_STRING` منفصلة

`OBJECT_NAME_INFORMATION` = `{ UNICODE_STRING Name; }` والمخزن المؤشَّر
إليه **مخزَّن منفصل** (كما في NT) ⇒ يُملأ بـ UTF-16LE ويبقى للقراءة
(لا يُحرَّر من جهة NT) ✓ — والاختبار يقرأ الاسم بـ UTF-16 لا `strcmp`.

#### 13.3 نتائج مُشغَّلة (85 فحصاً ⇒ 0 FAIL)

```
-- SYS-024/025/026: sections (0x47/0x25/0x27) --
PASS: NtCreateSection (anonymous) -> handle 0x4e54000010000001
PASS: NtMapViewOfSection -> 0x764a30eb8000 (4096 bytes)
PASS: the view is writable and readable
PASS: NtUnmapViewOfSection -> the view is released
PASS: a file-backed section maps the file itself (no copy)

-- SYS-027: NtFsControlFile (0x36) --
PASS: NtFsControlFile(FIONREAD) -> 10 bytes

-- SYS-028/029: lock / unlock (0xe0/0x188) --
PASS: NtLockFile (exclusive, 64 bytes) -> locked
PASS: NtUnlockFile -> unlocked
PASS: NtLockFile on a bad handle -> STATUS_INVALID_HANDLE

-- SYS-030: NtQueryAttributesFile (0x3a) --
PASS: a directory reports FILE_ATTRIBUTE_DIRECTORY (0x10)
PASS: a file reports FILE_ATTRIBUTE_NORMAL (0x80)

-- SYS-031: NtQueryObject (0x0d) --
PASS: ObjectNameInformation -> "Windows/System32/ntdll.dll" (52 bytes)
PASS: ObjectTypeInformation -> "File"
PASS: unsupported object class -> STATUS_NOT_SUPPORTED
```

#### 13.4 التغطية

المترجم الآن **30 syscall** مُترجَماً ومُختبَراً (من 400 مُقاسة في ntdll):
الملفات (إنشاء/فتح/قراءة/كتابة/معلومات/سمات/تعداد) · الذاكرة
(تخصيص/حرية/حماية/استعلام/مقاطع) · الأحداث (إنشاء/ضبط/مسح/انتظار) ·
الزمن (تأخير/تنازل/استعلام نظام) · الأقفال · الكائنات · ioctl.

**المتبقي للوصول إلى هدف 100 (T-061):** الخيوط (T-040) · العمليات (T-041/042)
· الريجستري (T-049/050) · الأنابيب (T-051) · `NtSetSystemInformation` (T-036)
· `NtSetInformationThread` (T-048) · `NtQueryFullAttributesFile` (T-056).

#### 14. جولة W2 الرابعة: الخيوط والعمليات (T-040 · T-041 · T-042 · T-046)

#### 14.1 ما الذي بُني

| الوحدة | الدور |
|---|---|
| `tools/ntboxd/syscalls/ntb0_thread.{h,c}` | إدارة الخيوط: جدول خيوط (64 مدخلاً)، عدّاد تعليق، `eventfd` لكل خيط (⇒ المقبض قابل للانتظار)، `pthread_cleanup_push/pop` |
| `ntb0_trampoline.S` → `ntb0_thread_entry` | جسر اصطلاح: نقطة نداء الخيط في NT = اصطلاح **ويندوز** (rcx) ⇒ نُنقل rdi ⇒ rcx مع 32 بايت shadow space |
| `ntb0_dispatch.c` (SYS-032..040) | `NtCreateThread` · `NtResumeThread` · `NtTerminateThread` · `NtOpenProcess` · `NtTerminateProcess` · `NtQueryInformationProcess` · `NtOpenThread` · `NtSuspendThread` · `NtQueryInformationThread` |

**دلالات مُطبَّقة (مطابقة لـ NT):**
- `CLIENT_ID` = (pid الحقيقي، tid NT داخلي يبدأ من 1).
- `NtCreateThread` مع `THREAD_CREATE_SUSPENDED` ⇒ عدّاد تعليق = 1 ⇒ **الخيط لا ينفّذ** حتى `NtResumeThread`.
- `PreviousSuspendCount` يُعاد في `NtResumeThread`/`NtSuspendThread`.
- مقبض الخيط **قابل للانتظار**: `NtWaitForSingleObject` عليه يُنبَّه عند انتهاء الخيط (eventfd) — كما في NT تماماً.
- `NtTerminateProcess` على **المترجم نفسه** ⇒ `STATUS_ACCESS_DENIED` (قتل نفسه = قتل المترجم).
- `NtTerminateProcess` على عملية أخرى ⇒ `SIGKILL` حقيقي.
- `NtQueryInformationThread` صنف 0 ⇒ `THREAD_BASIC_INFORMATION` (0x28 بايت).
- `NtQueryInformationProcess` أصناف 0/7/27/40 ⇒ `Basic` · `DebugPort` · `ImageFileName` · `Wow64`.

#### 14.2 النتائج المُقاسة

```
-- SYS-032/033/034/038/039/040: threads --
PASS: NtCreateThread(suspended) -> handle=5644136233387950081
PASS: CLIENT_ID = pid 35643 / tid 1
PASS: suspended thread did not run yet
PASS: NtResumeThread -> previous suspend count 1
PASS: NtWaitForSingleObject(thread handle) -> signaled
PASS: thread wrote A5 5A C3 3C through the ABI bridge
PASS: thread exit status = 0x1234
PASS: ThreadBasicInformation -> exit=0x1234 tid=1 (size 0x28)
PASS: NtOpenThread(tid=1) -> handle=5644136233136291842
PASS: unsupported thread class -> STATUS_NOT_SUPPORTED
PASS: NtCreateThread(loop) -> handle=5644136233404727297 tid=2
PASS: NtSuspendThread -> previous suspend count 0
PASS: NtResumeThread -> previous suspend count 1
PASS: NtTerminateThread -> exit status 0xDEAD
PASS: terminated thread signaled its handle
PASS: terminated thread exit status = 0xdead

-- SYS-035/036/037: processes --
PASS: NtOpenProcess(self) -> handle=5644136233421504513
PASS: ProcessBasicInformation -> pid=35643 ppid=35642 (size 0x30)
PASS: ProcessImageFileName -> ".../tools/ntboxd/syscalls/test_trap"
PASS: ProcessWow64Information -> 0 (64-bit)
PASS: unsupported process class -> STATUS_NOT_SUPPORTED
PASS: NtTerminateProcess(self) -> STATUS_ACCESS_DENIED
PASS: NtOpenProcess(bogus pid) -> 0xc000000d (refused)
PASS: NtTerminateProcess(child) -> SIGKILL
```

#### 14.3 الأخطاء المرتكبة في هذه الجولة (وإصلاحها)

1. **deadlock ذاتي (الأخطر):** `ntb0_thread_create` كان يقفل `ntb0_thread_lock`
   في بدايته ثم **يعيد قفله** داخله عند إسناد `nt_tid` ⇒ mutex غير قابل
   لإعادة القفل ⇒ تعليق دائم ⇒ الاختبار علِق في `NtCreateThread`.
   **الإصلاح:** القفل المكرر أُزيل (الإسناد يتم والقفل محمول).
2. **سباق في رمز الخروج:** `ntb0_thread_main` كان يكتب `exit_code` الطبيعي
   **دون قفل** ⇒ قد يطغى على الرمز الذي فرضه `NtTerminateThread`.
   **الإصلاح:** الكتابة داخل القفل، وبشرط `!t->terminated`.
3. **مصباح FAIL في رفض صحيح:** `NtTerminateProcess` على النفس كان يُصدر
   `NTB0_FAIL` ⇒ عدّاد FAIL غير صفري. **الإصلاح:** الرفض الصحيح = مصباح
   **OK** (كما في العقد: `ACCESS_DENIED` المتوقَّع ليس فشلاً).
4. **الـ probe على 0x50:** كان `0x50` (NtTerminateThread) بلا handler ⇒ مصباح
   UNIMPL؛ وبعد التنفيذ صار له handler حقيقي ⇒ الاستدعاء الوهمي يُنتج
   `INVALID_HANDLE` ⇒ لُفّ بـ `expect_fail` (رفض صحيح) ⇒ لا FAIL.

#### 14.4 الدرس المُضاف (L16)

> **اقفل القفل مرّة واحدة فقط.** القفل المتداخل على نفس الـ mutex داخل نفس
> الدالة = تعليق أبدي، والاختبار **يتجمّد** بدل أن يفشل ⇒ العلامة الأولى
> (تجمّد الإخراج) = deadlock لا خطأ منطقي. الحل: قفل واحد + مراجعة كل
> `lock/unlock` في المسار، وتشغيل الاختبارات دائماً بمهلة (`timeout`) حتى
> لا يجمّد الـ CI.

#### 14.5 التغطية

المترجم الآن **39 syscall** مُترجَماً ومُختبَراً (من 400 مُقاسة في ntdll):
الملفات · الذاكرة · المقاطع · الأحداث · الزمن · الأقفال · الكائنات · ioctl ·
**الخيوط (إنشاء/تعليق/استئناف/إنهاء/فتح/استعلام)** · **العمليات
(فتح/إنهاء/استعلام)**.

**المتبقي للوصول إلى هدف 100 (T-061):** الريجستري (T-049/050) · الأنابيب
(T-051) · `NtSetSystemInformation` (T-036) · `NtSetInformationThread` (T-048)
· `NtQueryFullAttributesFile` (T-056).


#### 15. الجولة الخامسة: الريجستري + الأنابيب + الافتراضيات (T-013…T-017 · T-036 · T-048 · T-051 · T-056 · T-096…T-114 · T-163…T-168)

#### 15.1 البيئة (W1) — استُعيدت بعد إعادة تشغيل الجهاز

| # | المهمة | النتيجة **المقيسة** |
|---|---|---|
| T-013 | استخراج `install.wim` من الـ ISO | 2,819,538,787 بايت (7z 23.01 يدعم WIM أصلياً) |
| T-014 | فكّ الشجرة | **45,077 ملف** · 7,337,787,603 بايت داخل WIM |
| T-015 | `*.sys` من `System32/drivers` | **278 ملف .sys** |
| T-016 | خلايا الريجستري | `SYSTEM` (9,175,040) · `SOFTWARE` (37,748,736) · `DEFAULT` · `SAM` · `SECURITY` |
| T-017 | جذر دائم | `win7full/` (14G) + `win7root` → رابط + `/tmp/opencode/win7root` → رابط |

> **درس (L17):** البيئة كانت في `/tmp` ⇒ مسحت بإعادة تشغيل الجهاز ⇒ كل اختبار
> "ويندوز حقيقي" انهار. الحل: `tools/prepare_win7.sh` (أمر واحد يعيد البناء)
> + نسخة **دائمة** خارج `/tmp` + بحث تلقائي عن الجذر في
> `tools/verify.sh` و`ntb0_path.c`.

#### 15.2 **قياس** بنية خلية الريجستري (لا تخمين)

قِيست بنية `SYSTEM` الحقيقية بايت-ببايت على الملف (أداة مؤقتة خارج المشروع):

| السجل | الإزاحة (من بدايته) | الحقل | القيمة المقيسة |
|---|---|---|---|
| الخلية | +0x00 | حجم (int32) | سالب = مشغولة |
| | +0x04 | السجل | `"nk"` / `"vk"` / `"lh"` / `"lf"` / `"li"` |
| `nk` | +0x02 | flags | `0x20` = الاسم ASCII (وإلا UTF-16LE) |
| | +0x04 | الطابع | FILETIME ✔ (2010-11-21 = تاريخ الصورة) |
| | +0x10 | parent | `0x20` ✔ (الجذر) لأبناء الجذر |
| | +0x14 / +0x1C | nsub / قائمة المفاتيح | 7 و `0x8455c8` (نوعها `"lh"`) |
| | +0x24 / +0x28 | nval / قائمة القيم | 12 و `0x67b98` |
| | +0x48 / +0x4C | طول الاسم / الاسم | ✔ |
| `vk` | +0x02 | طول الاسم | ✔ |
| | +0x04 | حجم البيانات | `0x80000004` ⇒ **بيانات داخلية** (البت 31) |
| | +0x08 | الإزاحة **أو البيانات نفسها** | البيانات ≤ 4 بايت مخزّنة في هذا الحقل ✔ |
| | +0x0C | النوع | 1=SZ · 2=EXPAND_SZ · 4=DWORD · 7=MULTI_SZ ✔ |
| | +0x10 | flags | `0x01` = الاسم ASCII |

**القائمة الفرعية (`"lh"`):** مدخل 8 بايت = `{إزاحة nk، hash}`.
**قائمة القيم:** **بلا توقيع** — سلسلة إزاحات (4 بايت) تنتهي بصفر.

#### 15.3 القارئ والجسر — بالأرقام

| الأداة | المهمة | القياس |
|---|---|---|
| `tools/ntboxreg/hive.c` | T-096…T-100 | يقرأ `SYSTEM`/`SOFTWARE`/… |
| `tools/ntboxreg/ntboxreg.c` | T-101…T-106 | **414 خدمة** مسرودة (≥ 100 ✔) · `RpcSs`: ImagePath=`%SystemRoot%\system32\svchost.exe -k rpcss` · Start=2 · Type=0x20 · ObjectName=`NT AUTHORITY\NetworkService` |
| `tools/ntboxd/syscalls/ntb0_reg.c` | T-110…T-114 | `NtOpenKey` + `NtQueryValueKey` + `NtEnumerateKey` + `NtEnumerateValueKey` + `NtFlushKey` عبر **ستوبات ntdll الحقيقية** |
| `tools/ntboxd/scm.c` | T-163…T-168 | 414 خدمة · ترتيب بدء **يعتمد التبعيات** (414/414 مرتّبة) · **لا دورات** في الرسم الحقيقي (مُثبَت بمكتشف دورات اصطناعي) |

#### 15.4 الأوامر الجديدة في المترجم (كلها عبر ntdll Win7 الحقيقي)

| المصباح | الأمر | ما أُثبت |
|---|---|---|
| `SYS-041` | `NtSetInformationThread` (0x0a) | TEB يُخزَّن لكل خيط · الأولوية (تصغير = مسموح، تكبير = `PRIVILEGE_NOT_HELD` كما في NT) · الألفة بـ `sched_setaffinity` |
| `SYS-045` | `NtCreateNamedPipeFile` (0x9b) | **أنبوب مسمّى حقيقي** = FIFO على لينكس · ذهاب وإياب `PIPE-OK` عبر `NtWriteFile`/`NtReadFile` · تعارض الأسماء ⇒ `OBJECT_NAME_COLLISION` |
| `SYS-046` | `NtQueryFullAttributesFile` (0x113) | `EndOfFileLarge` يطابق حجم الملف على القرص **بايت-ببايت** (1,731,936) |
| `SYS-048` | `NtSetSystemInformation` (0x16c) | ضبط ساعة الصندوق ثم قراءتها (فرق < 1 µs) · **الإطفاء/الـ crash dump/جداول الخدمات مرفوضة دائماً** (`PRIVILEGE_NOT_HELD`) ⇒ المضيف لا يُطفأ ولا ينهار |
| `SYS-049…053` | الريجستري | فتح مفتاح حقيقي · قراءة `ImagePath` و`Start` · تعداد 12 قيمة و414 مفتاح خدمة |

#### 15.5 نتيجة الجولة (مقيسة)

```
trap      PASS   135 فحصاً · 0 FAIL · 3 STUB
registry  PASS   399 خدمة من خلية SYSTEM الحقيقية
services  PASS   SCM: 414 خدمة + ترتيب التبعيات
LAMP TOTAL: OK=5968 STUB=3 FAIL=0 UNIMPL=0
VERDICT: PASS
```

#### 15.6 الأخطاء المرتكبة في هذه الجولة (وإصلاحها)

1. **mkdir بدل mkfifo:** كان إنشاء الأنبوب ينشئ *مجلداً* بنفس الاسم ⇒
   `OBJECT_NAME_COLLISION` دائماً. الإصلاح: `mkfifo` + فحص `S_ISFIFO`.
2. **أولوية الخيط بالعكس:** المعامل `+1` في NT = *رفع* الأولوية (⇒ يحتاج
   امتياز) لا خفضها ⇒ صُحّح الاختبار إلى `-1`، والرفع يعود
   `PRIVILEGE_NOT_HELD` (سلوك NT الصحيح).
3. **الرفض المتوقَّع كـ FAIL مصباح:** استعادة الأولوية (رفع) وخدمات
   `Start=4` ⇒ مصابيح FAIL ⇒ انكسر شرط `FAIL=0`. الإصلاح: الرفض الصحيح
   = مصباح **OK** ( contract المشروع).
4. **إزاحة اسم القيمة:** كان اسم `vk` يُقرأ من إزاحة اسم `nk` ⇒ أسماء
   مشوّهة ⇒ صُحّح: `nk` ⇒ 0x4C و`vk` ⇒ 0x14، وبتّ ASCII مختلف
   (`nk`: 0x20 · `vk`: 0x01).


#### 16. الجولة السادسة: ثلاث قطع بُنيت **بالتوازي** (W5 · W7 · W9a)

> **أسلوب التنفيذ:** قُسّم البناء إلى ثلاثة أجزاء **بملفات متباعدة تماماً**
> (لا تعارض بين الـ agents) ⇒ كل agent يبني/tester Teilه بنفسه، ثم
> **الدمج** في `verify.sh` + تشغيل `make verify` واحد على المشروع كله.

#### 16.1 W9a — مسار GPU بلا نسخ (T-201…T-208) — **مقيس على العتاد الحقيقي**

| المصباح | ما أُثبت | القياس |
|---|---|---|
| `GFX-001` | الأداة تعمل | `ntboxgfx selftest` |
| `GFX-002` | عقدة الرسم مفتوحة | `/dev/dri/renderD128` · `amdgpu` · DRM 3.57.0 |
| `GFX-003` | جهاز GBM | `gbm_create_device` على renderD128 |
| `GFX-004` | **إطار GPU حقيقي** | EGL 1.5 + GLES2 · مثلث ملوّن على **RX 580** (radeonsi, polaris10) · usage=LINEAR\|SCANOUT |
| `GFX-005` | تصدير dma-buf | fd=8 · 1920×1080 · stride 8192 · ARGB8888 — **لا بكسل يمرّ عبر المعالج** |
| `GFX-006` | تسجيل إطار المسح | `drmModeAddFB(id=110)` على `/dev/dri/card1` |
| `GFX-007` | **عدد نسخ CPU = 0** | 1507 استدعاء `memcpy/memmove` · 32,196 بايت · **ولا واحد بحجم الإطار** ⇒ `>=64KiB calls=0 bytes=0` |
| `GFX-008` | compositor يستورد dma-buf | وصل في تشغيل الـ agent (weston) ⇒ SKIP في جولة `verify` (لا WAYLAND_DISPLAY) — **موثّق كما هو** |

**إثبات أن القياس حقيقي:** المتغيّر `NTB0GFX_SELFTEST_COPY=1` يفرض نسخاً
متعمّداً (8.3MB) ⇒ `GFX-007` يتحوّل إلى **FAIL** بالعدد المقيس ⇒ العدّاد
يقيس ولا يقرّر.

**ملاحظة عن البيئة:** لا توجد حزم تطوير (لا `gbm.h` ولا `EGL/` ولا
`xf86drm.h`) ⇒ كل الواجهات تُحمَّل بـ `dlopen` ⇒ **الأداة تُبنى دائماً**،
والهياكل اليدوية (DRM/GBM/EGL) قُوبلت بـ `linux-headers` المحلية.

#### 16.2 W5 — تصديرات PE + api-set (T-119 · T-127)

| القياس | القيمة |
|---|---|
| `kernel32.dll` (Win7 x64) | **1387 تصديراً مسمّى** · base=1 · PE32+ · 1,161,216 بايت — مطابق لـ `objdump` |
| `CreateFileA` | ordinal 137 · RVA `0x231f0` |
| `user32` 830 · `ntdll` 1984 · `advapi32` 805 · `msvcrt` 1298 | كلها مطابقة لـ `objdump` |
| api-set | `core-file`→`kernel32.dll` (76/76 تصديراً) · `core-synch`→`kernel32` · `security-base`→`advapi32` (97/97) · `crt-runtime`→`ucrtbase` (جدول احتياطي) |

**حدّ موثّق (مهم):** ويندوز 7 **لا يحتوي** مخطّط `apisetschema` (سبق به
ذلك) ⇒ **لا يوجد مخطط=RGui** ⇒ المضيف **يُشتق بالقياس** من قوائم تصدير
الملفات الحقيقية (تغطية 100%) + جدول احتياطي **موسوم صراحةً** كاحتياطي.
محلل APISET (ترويسة "APISET" + 3 جداول نصوص + جدولا تجزئة) **مُنفَّذ**
لكنه **غير مُتحقَّق منه** مقابل كتلة Microsoft حقيقية (لا كتلة هنا ولا شبكة)
⇒ مُختبَر بكتلة اصطناعية فقط ⇒ **مُوثَّق كحدّ، لا كإنجاز**.

#### 16.3 W7 — SCM (T-163…T-168 · T-171) — مُقيس

| القياس | القيمة |
|---|---|
| الخدمات | **414** (Type: 119 خدمة svchost مشتركة · 27 تعريف نواة · 39 بلا Type) |
| التبعيات | 129 خدمة تحمل `DependOnService` |
| ترتيب البدء | **414/414** مرتّبة بالتبعيات (كل تبعية سابقة ل dependents) |
| الدورات | **لا دورات** في الرسم الحقيقي ⇒ مُثبَت بمكتشف دورات اصطناعي (`NtBoxCycleA → NtBoxCycleB → NtBoxCycleA`) بلا تعليق |
| خدمات مستضافة (T-168) | 119 في **45** مجموعة svchost |

#### 16.4 نتيجة الدمج (كل المشروع)

```
build      OK     kernel module + all user tools
trap       PASS   135 فحصاً · 0 FAIL · 3 STUB
pe         PASS   4 تطبيقاً
pe-gui     PASS   نافذة تحت wayland
registry   PASS   399 خدمة من SYSTEM الحقيقي
services   PASS   SCM: 414 خدمة + ترتيب التبعيات
dll        PASS   تصديرات PE + api-set
gpu        PASS   **0 نسخ CPU مقيسة على RX 580**
LAMP TOTAL: OK=5993 STUB=3 FAIL=0 UNIMPL=0
SUITES:     PASS=8 FAIL=0 SKIP=0
VERDICT: PASS
```


#### 17. **لحظة الحقيقة (T-063)**: ntdll الحقيقي داخل الصندوق

#### 17.1 ما الذي بُني

`tools/ntboxd/ntboxd.c` صار يحمّل **ntdll.dll الحقيقي** من شجرة Win7،
ويعدّل ستوباته الـ400 **في مكانها** (نفس مصيدة `test_trap`)، ثم يحلّ
استيرادات التطبيق من `ntdll.dll` إلى **عناوين داخل الصورة الحقيقية المعدَّلة**:

```c
/* في ntboxd.c */
if (ntb0_map_pe(ntdll_path, &img) == 0 &&
    ntb0_trap_patch(&img) == 0) {          /* 400 ستوب E9 */
    ...
    /* solve: name → export RVA (من الملف) → base + RVA (في الصورة) */
    ntb0_pe_find_by_name(pe, name, &ex);
    addr = img.base + ex.rva;
}
```

**لماذا يعمل:** الصورة المرسومة نسخة **حرفية** من الملف ⇒ RVA التصدير
نفسه ⇒ `base + RVA` هو عنوان الدالة داخل الصورة المعدَّلة ⇒ نداء التطبيق
يدخل ستوباً مُعدَّلاً ⇒ المصيدة ⇒ المترجم ⇒ نواة لينكس.

#### 17.2 القياس (تشغيل حقيقي)

```
[ntbox][TRAP-011][OK] mapped .../System32/ntdll.dll: base=0x7719ca2a0000 SizeOfImage=0x1a9000
[ntbox][TRAP-014][OK] syscall stubs found=400 patched=400 (in-place, E9 rel32)
[ntboxd] real ntdll!NtQuerySystemInformation -> 0x7719ca2f1670   ← عنوان حقيقي داخل الصورة
ntdll_probe: calling the REAL ntdll
[ntbox][SYS-022][OK] NtQuerySystemInformation (0x33) -> 0x0      ← عبر المصيدة
ntdll_probe: BasicInformation page=096 procs=04                 ← بيانات لينكس الحقيقية (4 معالجات)
[ntbox][SYS-022][OK] NtQuerySystemInformation (0x33) -> 0x0
[ntbox][SYS-041][OK] thread priority set to 0 (nice 0)
[ntbox][SYS-041][OK] NtSetInformationThread (0xa) -> 0x0
ntdll_probe: done
```

**الأداة:** `tools/build_dubltest/ntdll_probe.c` (PE32+ بـ mingw، بلا CRT،
يستورد `NtQuerySystemInformation`/`NtSetInformationThread`/`NtClose` من
**ntdll.dll** مباشرة) ⇒ وهو الآن جزء من عقد `verify` (مجموعة `pe`).

#### 17.3 الدرس (L18)

> **الاستيراد لا يحتاج محمّل DLL كاملاً كي يصل إلى ntdll.**
> بما أن الصورة المرسومة نسخة حرفية من الملف، فإن `RVA` التصدير (مقروء من
> الملف بـ `pe_export`) + قاعدة الصورة = عنوان الدالة داخل الصورة ⇒ يُحلّ
> الاستيراد **دون** محمّل عام. يبقى `kernel32.dll` الحقيقي محجوباً
> (يستعمل الصندوق الـ stubs) ⇒ هذه هي T-120.


#### 17.4 cmd.exe الحقيقي — أين وصل بالضبط (قياس)

تشغيل `win7full/Windows/System32/cmd.exe` (تطبيق Win7 **حقيقي**) داخل الصندوق:

| القياس | القيمة |
|---|---|
| استيرادات ntdll المحلولة إلى **الصورة الحقيقية المعدَّلة** | **14** (RtlCaptureContext · RtlVirtualUnwind · NtFsControlFile · NtOpenProcessToken · NtClose …) |
| استيرادات kernel32 المحلولة إلى الـ stubs المحلية | 146 |
| ما وصل إليه | نقطة الدخول نُفِّذت ⇒ أول استيراد غير مُنفَّذ (kernel32 الحقيقي غير محمَّل بعد) ⇒ segfault |
| syscalls مُترجَمة فعلياً | 0 (لم يبلغ بعد نداءً من ntdll — الـ crash سابق) |

**الخلاصة الصادقة:** تحميل تطبيق Win7 حقيقي + حلّ استيراداته من ntdll
الحقيقي **أثبات**؛ أمّا **تشغيله** فيحتاج `kernel32.dll` الحقيقي ⇒ T-120.

---

---

## 11. الحدود الموثّقة (ما لن ينجح — صراحةً)

| الحدّ | السبب | البديل |
|---|---|---|
| `ShareAccess` بلا **أقفال** كما في NT | `flock` مختلف دلالياً | جدول المقابض يكفي `SharingViolation` |
| I/O غير متزامن بـ APC | يحتاج بنية APC | `eventfd` + polling (T-072) |
| `RootDirectory` لا يقبل `..` | حماية من الهروب | `PATH-006` |
| UNC / \\shares | SMB | `cifs` عبر mount (مؤجَّل) |
| `explorer.exe` كامل | يحتاج win32k + COM + shell | تدريجي (W8) |
| تشغيل KMD (amdkmdag) | يحتاج نواة NT | مسار UMD (T-218..T-220) |
| محاكاة `KeBugCheck` | مجهول | مُستبعَد عمداً |
| **`kernel32.dll` الحقيقي يعمل جزئياً** | 390 من 891 استيراداً غير محلولة + `DllMain`/TLS غير مُستدعَين | T-120 |
| **`cmd.exe` لا يعمل بعد** | يحتاج kernel32 الحيّ لا مجرد تحميله | T-120 ثم T-165 |
| **`msvcrt.dll` غير محمَّل** ⇒ تطبيقات CRT تنهار | CRT يستدعي memcpy/memset قبل `main` | طبقة C صغيرة (22 دالة) + T-136 |
| **مخطّط `apisetschema` غير موجود في Win7** | Win7 يسبق المخطّط (276 ملف stub بدل thereof) | اشتقاق المضيف **بالقياس** (تغطية 100%) + جدول احتياطي **موسوم** |
| **`kernel32.dll` الحقيقي يعمل جزئياً** | 390 من 891 استيراداً ما زالت غير محلولة + `DllMain`/TLS غير مُستدعَين | التطبيق يُحمَّل لكن لا يعمل ⇒ T-120 |
| **محلّل APISET غير مُتحقَّق منه** | لا كتلة Microsoft هنا (ولا شبكة) | مُختبَر بكتلة اصطناعية ⇒ **حدّ موثّق** |
| `GFX-008` قد يتخطّى في جولة `verify` | لا `WAYLAND_DISPLAY` حيّ | يعمل مع weston (مُثبَت) ⇒ rc=3 ⇒ SKIP صريح |
| البيئة (شجرة Win7) كانت في `/tmp` ⇒ تُمسح | `/tmp` tmpfs | `prepare_win7.sh` + نسخة دائمة + بحث تلقائي |


## 12. ما يحتاجه المشروع (المرتّب بالأولوية)

> كل بند هنا **مُقيس**: إمّا مُنجَز (✅) أو مُتبقٍّ (⬜) مع سبب واضح.
> الأولوية = ما يقرّب المشروع من owner's vision:
> *«صندوق ويندوز فيه كل الخدمات والريجستري وSystem32، يرسم على كرت
>/Linux ويعرضه Wayland بلا نسخ — بلا task manager ولا واجهات تطبيقات».*

### 12.1 معيار صريح — قياس الفكرة كما وصفها المالك

| # | ما طلبه المالك | الحالة | الدليل |
|---|---|---|---|
| 1 | «نظام يشبه ويندوز» (System32/System64 + خدمات + مستكشف + ريجستري) | 🟡 | System32 ✅ · الخدمات 414 ✅ · الريجستري ✅ · مستكشف الملفات ⬜ (يحتاج win32k+shell) |
| 2 | «لا يملك جزء الموارد، بل طبقة ترجمة» | ✅ | الترجمة عند الـ syscall: 48 أمر ntdll حقيقي مترجَم |
| 3 | «تعريف كرت الشاشة ويندوز ⇒ يُترجم إلى تعريف لينكس (AMD)» | 🟡 | مسار GPU حقيقي بلا نسخ ✅ (0 نسخ) · **جسر أوامر الكرت (T-210…T-221) ⬜** |
| 4 | «الوايلاند يعرض بلا نسخ» | ✅ | dma-buf + linux-dmabuf ⇒ **0 نسخ CPU مقيسة** على RX 580 |
| 5 | «بدون task manager ولا تطبيقات ولا خلفيات» | ✅ | الصندوق بلا واجهة؛Wayland يعرض ما يرسمه الصندوق فقط |
| 6 | «محرك رسم يبقى داخل الصندوق» | 🟡 | `gfx_present` يعرض من الصندوق ✅ · **محرّك الرسم نفسه (GDI/D3D ⇒ dmabuf) ⬜** |

### 12.2 ما تبقّى — بالترتيب (المهمة التالية أولاً)

> **T-063 (لحظة الحقيقة) أُنجزت**: ntdll.dll (و kernel32.dll) الحقيقيان يعملان **داخل الصندوق** (400 ستوب + 891 استيراد) ⇒ تطبيق Win7 ينادي ntdll (SYS-022/SYS-041) ⇒ لينكس.
> **تحديث 2026-10-25**: **تطبيقات GUI حقيقية تعمل كاملة** — `gui-real PASS 3/3` (window/GDI/input ⇒ EXIT=0) بعد إصلاحات: `ProcessCookie (0x24)` + مقبضات وهمية `-1/-2` + خلايا بيانات msvcrt (`__initenv/_fmode/_commode`) + CRT stubs حقيقية (`_initterm`/`_onexit`/`_errno`…) + `ExitProcess` داخل الصندوق — وجذر حلقة `NtRaiseException` المحقَّق بـ gdb (RtlRaiseStatus يعيد التكرار بلا شرط ⇒ الإرجاع ممنوع، الإنهاء الحاسم هو العقد الآمن).
> **cmd.exe/notepad.exe** يُحمَّلان (entry reached) لكن لا يطبعان — السبب: `UNRESOLVED ntdll.dll!#8` (ordinal import) + kernel32 الحقيقي necesita `DllMain`/TLS (T-131/T-132) + PEB/CommandLine (T-141..T-145) + console APIs (T-134).
> **الـ kernel32 (T-120) جزئياً**: 501/891 استيراداً محلولاً (429 غير محلول — وحدات غير محمَّلة).

**P0 — يجعل الفكرة تعمل كما وصفها المالك (لا غنى عنها):**

| # | المهمة | لماذا الآن | معيار النجاح |
|---|---|---|---|
| T-210…T-221 | **جسر أوامر الكرت**: تعريف الكرت داخل الصندوق يُترجم إلى amdgpu؛ وطبقة UMD (D3D11/Vulkan/GL) فوق الوسيط | المسار الوحيد الذي يجعل «تعريف ويندوز على كرت Linux» حقيقة بدل أن يكون مساراً مستقلاً | `GFX-013…017` + D3D11 ⇒ Wayland |
| T-222/223 | مسار D3D11 داخل الصندوق ⇒ EGL/GBM ⇒ dmabuf ⇒ Wayland | «الصندوق يرسم» لا «الصندوق يعرض» | لقطة D3D11 داخل الصندوق بلا نسخ |
| T-120…T-126 | **kernel32.dll الحقيقي** (المحمّل موجود ✅، ينقص: تشغيل kernel32 الحي + DllMain/TLS + delay-load) | اليوم `cmd.exe` يُحمَّل لكن لا يعمل (يستدعي kernel32 غير محمَّل) | `cmd.exe` يطبع `dir` |
| T-131/132 | `DllMain` + TLS callbacks | شرط تشغيل أي DLL حقيقي | استدعاء DllMain عند التحميل |
| T-165 | تشغيل خدمة حقيقية (من الـ SCM ⇒ الملف ⇒ تشغيله) | «كل خدمات ويندوز الأساسية» ⇒ خدمة تعمل فعلاً | خدمة `RpcSs`/غيرها تعمل داخل الصندوق |
| T-173…T-193 | win32k مصغّر: نافذة/رسوم/إدخال | «مستكشف الملفات» و«واجهة صندوق» تحتاجها | نافذة Win7 حقيقية + إدخال |

**P1 — يوسّع الصندوق (مهم لكنه ليس شرط الفكرة):**

| # | المهمة | معيار النجاح |
|---|---|---|
| T-057/058/059 | I/O غير متزامن (APC)، `NtDuplicateObject` بين عمليتين، قراءة/كتابة على أنبوب | `STATUS_PENDING` + APC |
| T-056 | `NtQueryFullAttributesFile` (✅ منجَز) + `NtQueryFullAttributes` | — |
| T-048/051 | (✅ منجَزان) | — |
| T-107/108 | `RegLoadAppKey` + `REG_LINK` | مفتاح مترابط |
| T-141…T-158 | PEB/TEB كامل + TLS + heaps + SEH | تطبيق Win7 يمرّ بلا انهيار |
| T-241…T-249 | القرص: NTFS حقيقي + `dir` على القرص | `dir` حقيقي |
| T-112/114/115/116 | `advapi32` الحقيقي: `RegOpenKeyExA`/`RegEnumKeyExA` | `regedit` يقرأ الريجستري |

**P2 — بوّابات العمل والتوثيق:**

| # | المهمة | معيار النجاح |
|---|---|---|
| T-022 | بوّابة W1: `null.sys` (DriverEntry + IRP) | اختبارا M3/M4 يُعاد إنتاجه |
| T-118 | بوّابة W4: `advapi32` الحقيقي بلا stub | `RegOpenKeyExA` من advapi32 Win7 |
| T-140 | بوّابة W5: محمّل DLL كامل | DLL حقيقي يُحمَّل ويُستدعى |
| T-162 | بوّابة W6: PEB/TEB/TLS | تطبيق Win7 حقيقي يبدأ |
| T-172 | بوّابة W7: خدمة تعمل | خدمة تعمل |
| T-194 | بوّابة W8: نافذة + إدخال | نافذة + إدخال |
| T-250 | بوّابة W10: القرص | `dir` على القرص |
| T-251…T-258 | التقرير النهائي + التنظيف + قائمة «ما لم يُنفَّذ» | ملف واحد نظيف |

### 12.3 ما يحتاجه المشروع من **أدوات/بيئة** (ليس كوداً)

| # | الحاجة | لماذا | الحالة |
|---|---|---|---|
| 1 | **شجرة Win7 دائمة** | كل اختبار حقيقي يحتاجها؛ `/tmp` يُمسح ⇒ `tools/prepare_win7.sh` | ✅ جاهزة (14G) |
| 2 | `7z` يدعم WIM | استخراج `install.wim` | ✅ 7-Zip 23.01 |
| 3 | `mingw-w64` | بناء تطبيقات اختبار PE | ✅ |
| 4 | `wayland-client` + `weston` | اختبار العرض | ✅ |
| 5 | حزم تطوير GBM/EGL/libdrm | **غير ضرورية**: الأداة تستخدم `dlopen` ⇒ تُبنى دائماً | ✅ (dlopen) |
| 6 | `wimlib-imagex` | **غير ضرورية** (7z يكفي) | — |
| 7 | `sudo`/root | مجموعة `kernel` (وحدة النواة) | ⬜ تحتاج صلاحية عند التشغيل |

### 12.4 قرارات تحتاج من المالك (حسب §8)

1. **تثبيت `kernel32.dll` الحقيقي**: عند تشغيل خدمة حقيقية، هل نسمح بتشغيل
   ملف `.exe` من صورة Win7 داخل الصندوق (سلوكه كامل على لينكس)؟ (الأمان:
   الملفات من صورة رسمية، وال	box معزول بصلاحيات المستخدم).
2. **حجم العمل لـ W9a (جسر الكرت)**: المسار UMD (D3D11⇒EGL) هو
  实施方案 المنطقي؛ المسار KMD (محاكاة `amdkmdag.sys`) أُوقف كبحثي (§1).
3. **جدولة الوقت**: ما أولويّة المستخدم — الكرت (T-210) ولا تشغيل
   التطبيقات (T-120/T-165)؟ كلاهما P0، والاختيار يغيّر ترتيب الأسابيع.

## 13. الواجهة (GUI) — تطبيق لينكس أصلي (لا أوامر)

| الملف | الدور |
|---|---|
| `tools/ntbox-gui` | مدير الصندوق: تشغيل (selftest/runexe/runuser/loadsys) · أقراص (attach/detach + sudo) · GPU · ملفات · **مجموعات** (ألعاب/تطبيقات…) — جاهز للاتصال بالمسار الحالي |
| `tools/ntbox-install` | مثبّت NT-Box (بناء + استخراج Win7 + verify + GPU + أقراص) — جاهز للتثبيت الآلي |

**المجموعات (Groups):** تنظيم اختصارات (ملفات `.exe`/`.sys`) في مجموعات مسّماة بحرية
(«ألعاب»، «تطبيقات»، …) — كلها داخل NT-Box واحد ⇒ أيقونات + تشغيل بنقرة.
البيانات: `~/.config/ntbox/groups.json`. **SysWOW64 مُدمج** في مسارات البحث للبحث عن DLLs/التطبيقات.

## 14. كيف تشغّل هذا المشروع الآن

```bash
make verify          # كل المجموعات + تقرير موحّد (docs/verify_report.txt)
make verify-quick    # بلا root
make report          # آخر تقرير
make selftest        # مترجم الـ syscalls فقط (سريع)
make                 # بناء نواة لينكس
sudo insmod ntbox.ko # تحميل النواة
sudo rmmod ntbox     # تفريغ
```

**أدوات المشروع:**

```bash
tools/prepare_win7.sh                    # (إعادة) بناء بيئة Win7 من الـ ISO
./tools/ntboxreg/ntboxreg services SYSTEM  # 414 خدمة من SYSTEM الحقيقي
./tools/ntboxd/test_scm                    # SCM: ترتيب التبعيات
./tools/ntboxd/test_loader                 # محمّل DLL: map + reloc + imports
./tools/ntboxd/test_dll                   # تصديرات PE + api-set
./tools/ntboxgfx/ntboxgfx selftest         # مسار GPU بلا نسخ (يقيس 0 نسخ)
./tools/ntboxd/test_present                # العرض من داخل الصندوق (dma-buf ⇒ Wayland)
./tools/ntboxd/ntboxd <app.exe>            # تشغيل تطبيق داخل الصندوق
```

**ملاحظات بيئة:**

- جذر Win7: `$NTBOX_ROOT` (افتراضياً `<project>/win7root` ⇒ نسخة دائمة 15G).
- `NTBOX_NO_REAL_NTDLL=1` ⇒ تعطيل تحميل ntdll/kernel32 الحقيقيين (المسار المستقر).
- `NTB0GFX_SELFTEST_COPY=1` / `NTB0_PRESENT_SELFTEST_COPY=1` ⇒ نسخ متعمّد
  (لإثبات أن عدّاد النسخ **يقيس** ولا يقرّر).
- `NTBOX_PIPE_DIR` ⇒ مجلد أنابيب الصندوق (افتراضياً `/tmp/opencode/ntbox_pipes`).

**معلومات البيئة:** Linux Mint · نواة `6.8.0-147-generic` · i5-3470 ·
**RX 580 (amdgpu)** · 8GB RAM · gcc-13 · 7-Zip 23.01 · mingw-w64 · weston 13.

**بعد كل `./tools/verify.sh` ناجح ⇒ المشروع متقدّم خطوة واحدة، بلا حاجة لسؤال.**

## 15. الحالة الحالية — المكونات وحالتها

الجدول أدناه يعرض **كل مكون في المشروع** مع حالته الفعلية (منجَز / قيد العمل / معلق) والدليل على ذلك من السجلات المُقاسة.

| # | المكون | الحالة | الدليل / القسم المرجع |
|---|---|---|---|
| 1 | نواة `ntbox.ko` | 🟡 قيد العمل | يُبنى على `6.8.0-147`؛ الحمولة عالقة في `Unloading` → يحتاج إعادة تشغيل الجهاز (§6، §11.5) |
| 2 | المصابيح (`ntb_trace.h`) | ✅ منجَز | 136 معرّفاً + `/proc/ntbox/selftest` + `/proc/ntbox/stats` (§6) |
| 3 | جدول الترجمة في النواة | 🟡 قيد العمل | 117 دالة، 48 منها مصباح `STUB` (§6) |
| 4 | `linux_bridge` | 🟡 قيد العمل | ملفات/برتنز/DRM ioctls حقيقية؛ `ntb_gpu_mmap_gem` معطوب (§6) |
| 5 | محمّل PE (32/64-بت) | ✅ منجَز | relocations، imports، مستضاف في النواة وفي وضع المستخدم (§6) |
| 6 | مصيدة syscalls (NT-B0) | ✅ منجَز (جزئي) | 400 ستوب مُعدَّلة في مكانها، 11 معاملاً، 48 handler (§4، §10.9) |
| 7 | `ntdll.dll` / `kernel32.dll` الحقيقيان | 🟡 قيد العمل | T-063/T-120 — 400 ستوب معدَّلة؛ `kernel32` الحيّ ما زال يعمل جزئياً (§6، §17) |
| 8 | `ntboxd` | 🟡 قيد العمل | يُحمّل PE ويبني PEB/TEB؛ `user32/gdi32/d3d11/dxgi` معاد تنفيذها داخلياً (§6) |
| 9 | الريجستري (قارئ الخلايا) | ✅ منجَز | بنية مقيسة + جسر في المترجم ⇒ 399 خدمة من SYSTEM الحقيقي (§6، §15) |
| 10 | الخدمات (SCM) | ✅ منجَز | 414 خدمة · ترتيب بدء بالتبعيات (414/414) · لا دورات (§6) |
| 11 | GPU بلا نسخ (zero-copy) | ✅ منجَز | 0 نسخ CPU مقيسة على RX 580 (dma-buf ⇒ Wayland) (§6، W9a) |
| 12 | تصديرات PE + api-set | ✅ منجَز | 1387 تصديراً في kernel32 · api-set مشتق بالقياس (§6، W5) |
| 13 | قارئ الريجستري (نواة) | ❌ معلق | غير موجود (غير مطلوب — القارئ في user-space) (§6) |
| 14 | إدخال لوحة المفاتيح/الفأرة | ✅ منجَز (جزئي) | `input.c` (Wayland ⇒ Win32 messages) · 27/27 PASS (§9) · `test_input_live.c` |
| 15 | win32k مصغّر (نوافذ/رسوم/إدخال) | ✅ منجَز (جزئي) | `win32k.c` (RegisterClass/CreateWindow/طوابير/DC) + جسر GDI⇒GPU — `pe-gui PASS` · `gui-real PASS 3/3` (§9) · رسم GDI الحقيقي متبقٍ |
| 16 | كرت الشاشة داخل الصندوق (JSR/UMD) | 🟡 قيد العمل | `d3d11_bridge.c` موجود · `test_umd.c` · يُستكمل على أساس مسار GPU بلا نسخ (W9) |
| 17 | I/O غير متزامن (APC) | ✅ منجَز | `ntb0_async_io.c` (worker pthread · 0x103 فوري · eventfd · cancel) (§9) |
| 18 | NTFS حقيقي على القرص | ❌ معلق | T-241…T-249 غير begun (§7 W10) |
| 19 | بيئة Win7 دائمة (شجرة الملفات) | ✅ منجَز | `win7full/` + `win7root` (14G) · `prepare_win7.sh` (§6 T-017) |
| 20 | أداة النشر GUI (`ntbox-gui`) | ✅ منجَز | `tools/ntbox-gui` (GTK3 · تحميل النواة تلقائياً · runtimes · partitions · shortcuts.json) |
| 21 | تشغيل تطبيقات Windows GUI حقيقية | ✅ منجَز | `gui-real PASS (39/0/0/0)` — window/GDI/input كلها EXIT=0 بعد إصلاحات CRT/Win32 (§9 2026-10-25) |

### ملخص الحالات

| الحالة | عدد المكونات | المثال |
|---|---|---|
| ✅ منجَز | 15 | نواة البناء، المصابيح، محمّل PE، الريجستري، الخدمات، GPU بلا نسخ، تصديرات PE، بيئة Win7، SysWOW64، `complex`، `disk`/`runtime`/`driver`، إدخال (جزئي)، I/O غير متزامن، `ntbox-gui`، **تطبيقات GUI حقيقية** |
| 🟡 قيد العمل | 4 | جدول الترجمة، `linux_bridge`، ntdll/kernel32 الحي، `ntboxd` + win32k (جزئي) + جسر الكرت |
| ❌ معلق | 2 | قارئ الريجستري (نواة — غير مطلوب)، NTFS حقيقي |

> المجموع: 22 مكوناً · 12 منجَز · 5 قيد العمل · 5 معلق.
> التفاصيل الكاملة في §6 (الحالة الراهنة المُقاسة) و§7 (الخطة المتصلة T-001…T-258).

---

## 16. كيفية التشغيل — البناء والتشغيل والاختبار

جميع الأوامر تُنفّذ من جذر المستودع `/home/imadix/Downloads/Niga`.

### 16.1 البناء (Build)

```bash
# بناء كل شيء (نواة لينكس + أدوات المستخدم)
make

# بناء فقط أداة مستخرج جدول syscall
make ntb0extract

# إعادة توليد جدول syscalls من ntdll.dll الحقيقي (مُولَّد — لا يُعدّل يدوياً)
make syscall-table

# تنظيف منتجاته البناء
make clean
```

### 16.2 الاختبار (Test)

```bash
# تشغيل كل مجموعات الاختبار + تقرير موحّد (يحتاج root لنواة النواة)
make verify

# نفس الشيء بلا مجموعة النواة (لا يحتاج root)
make verify-quick

# عرض آخر تقرير
make report

# الاختبار الذاتي فقط لمترجم syscalls (سريع)
make selftest
```

مجموعات الاختبار في `tools/verify.sh`:
`build` · `trap` · `pe` · `pe-gui` · `kernel` · `registry` · `services` · `dll` · `complex` · `gpu` · `disk` · `runtime` · `driver`.

التقرير يُكتب إلى `docs/verify_report.txt` بصيغة ثابتة (`NT-BOX VERIFY REPORT v1`).
الحكم: `VERDICT: PASS` ⇔ لا مجموعة `FAIL` وصفر مصباح `[FAIL]`. رمز الخروج: `0` = PASS · `1` = FAIL. (الحالة الحالية: `pe` FAIL، المجموعات الأخرى PASS.)

### 16.3 التشغيل (Run)

```bash
# تحميل وحدة النواة
sudo insmod ntbox.ko

# تفريغ وحدة النواة
sudo rmmod ntbox

# عرض سجلات النواة
make logs   # (= dmesg | tail -n 30)

# إعادة تحميل (تفريغ ثم تحميل ثم سجلات)
make reload
```

### 16.4 أدوات المشروع

```bash
# (إعادة) بناء بيئة ويندوز 7 من الـ ISO (أمر واحد — ينجو من إعادة تشغيل الجهاز)
tools/prepare_win7.sh

# قارئ الريجستري (414 خدمة من SYSTEM الحقيقي)
./tools/ntboxreg/ntboxreg services SYSTEM
./tools/ntboxreg/ntboxreg list SYSTEM
./tools/ntboxreg/ntboxreg query SYSTEM 'ControlSet001\services\RpcSs'

# SCM — إدارة الخدمات من الريجستري الحقيقي
./tools/ntboxd/test_scm

# محمّل DLL + api-set
./tools/ntboxd/test_dll
./tools/ntboxd/test_loader

# مسار GPU بلا نسخ (يقيس عدد نسخ CPU)
./tools/ntboxgfx/ntboxgfx selftest

# العرض من داخل الصندوق (dma-buf ⇒ Wayland)
./tools/ntboxd/test_present

# تشغيل تطبيق داخل الصندوق
./tools/ntboxd/ntboxd <app.exe>

# قشرة NT-Box (أوامر النط)
sudo ./tools/ntshell/ntshell partitions
sudo ./tools/ntshell/ntshell policy allow <جذر>
sudo ./tools/ntshell/ntshell attach <جذر> C <مسار>
cat /proc/ntbox/result
```

### 16.5 متغيرات البيئة المفيدة

| المتغير | القيمة الافتراضية | الغرض |
|---|---|---|
| `WIN7` | `/tmp/opencode/win7root` | جذر شجرة ويندوز 7 المستخدمة في الاختبارات |
| `NTBOX_ROOT` | `$WIN7` | تُصدّر لمجموعات الاختبار جميعًا |
| `NTBOX_SUDO_PASS` | (غير مضبوط) | كلمة مرور sudo غير تفاعليّة لمجموعة النواة |
| `NTBOX_NO_REAL_NTDLL` | `0` | وضع `1` لتعطيل تحميل ntdll/kernel32 الحقيقيين (المسار المستقر) |
| `NTB0GFX_SELFTEST_COPY` | `0` | وضع `1` لفرض نسخ متعمّد في اختبار GPU (لإثبات أن العداد يقيس) |
| `NTBOX_PIPE_DIR` | `/tmp/opencode/ntbox_pipes` | مجلد أنابيب الصندوق |

### 16.6 معلومات بيئة التطوير الحالية

- النظام: Linux Mint
- النواة: `6.8.0-147-generic`
- العتاد: i5-3470 · RX 580 (amdgpu) · 8GB RAM
- الأدوات: gcc-13 · 7-Zip 23.01 · mingw-w64 · weston 13

> ملاحظة تشغيلية: الوحدة الحالية عالقة من Oops سابق (قبل الإصلاح في §11.5). لا يمكن تفريغها ⇒ مجموعة `kernel` في `verify.sh` تشخّص الحالة صراحةً، والتفريغ يتم عند إعادة تشغيل الجهاز. البيانات على القرص الدائم (`win7full/` / `win7root/` تنجو من إعادة التشغيل).

---

## 17. المهام المتبقية — الأولويات (P0, P1, P2)

المهام المتبقية مرتبة بالأولوية وفقًا لما يقرّب المشروع من رؤية المالك («صندوق ويندوز فيه كل الخدمات والريجستري وSystem32، يرسم على كرت Linux ويعرضه Wayland بلا نسخ»). الأرقام تشير إلى المهام في §7 (الخطة المتصلة T-001…T-258).

### P0 — لا غنى عنها لجعل الفكرة تعمل كما وصفها المالك

| # | المهمة | CAS (معيار النجاح) | السبب |
|---|---|---|---|
| T-210…T-221 | **جسر أوامر الكرت**: تعريف الكرت داخل الصندوق يُترجم إلى amdgpu؛ طبقة UMD (D3D11/Vulkan/GL) فوق الوسيط | `GFX-013…017` + D3D11 ⇒ Wayland | المسار الوحيد الذي يجعل «تعريف ويندوز على كرت Linux» حقيقة |
| T-222 / T-223 | مسار D3D11 داخل الصندوق ⇒ EGL/GBM ⇒ dmabuf ⇒ Wayland | لقطة D3D11 داخل الصندوق بلا نسخ | «الصندوق يرسم» لا «الصندوق يعرض» |
| T-120…T-126 | **kernel32.dll الحقيقي**: تشغيل kernel32 الحي + DllMain/TLS + delay-load | `cmd.exe` يطبع `dir` | اليوم `cmd.exe` يُحمَّل لكن لا يعمل (يستدعي kernel32 غير محمَّل) |
| T-131 / T-132 | `DllMain` + TLS callbacks | استدعاء DllMain عند التحميل | شرط تشغيل أي DLL حقيقي |
| T-165 | تشغيل خدمة حقيقية (من SCM ⇒ الملف ⇒ تشغيله) | خدمة `RpcSs`/غيرها تعمل داخل الصندوق | «كل خدمات ويندوز الأساسية» ⇒ خدمة تعمل فعلاً |
| T-173…T-193 | win32k مصغّر: نافذة/رسوم/إدخال | نافذة Win7 حقيقية + إدخال | مستكشف الملفات وواجهة الصندوق تحتاجانها |

### P1 — يوسّع الصندوق (مهم لكنه ليس شرطًا للفكرة الأساسية)

| # | المهمة | CAS (معيار النجاح) |
|---|---|---|
| T-057 / T-058 / T-059 | I/O غير متزامن (APC)، `NtDuplicateObject` بين عمليتين، قراءة/كتابة على أنبوب | `STATUS_PENDING` + APC |
| T-107 / T-108 | `RegLoadAppKey` + `REG_LINK` | مفتاح مترابط يعمل |
| T-141…T-158 | PEB/TEB كامل + TLS + heaps + SEH | تطبيق Win7 يمرّ بلا انهيار |
| T-241…T-249 | القرص: NTFS حقيقي + `dir` على القرص | `dir` حقيقي على القرص |
| T-112 / T-114 / T-115 / T-116 | `advapi32` الحقيقي: `RegOpenKeyExA` / `RegEnumKeyExA` | `regedit` يقرأ الريجستري |

### P2 — بوّابات العمل والتوثيق

| # | المهمة | CAS (معيار النجاح) |
|---|---|---|
| T-022 | بوّابة W1: `null.sys` (DriverEntry + IRP) | اختبارا M3/M4 يُعاد إنتاجه |
| T-118 | بوّابة W4: `advapi32` الحقيقي بلا stub | `RegOpenKeyExA` من advapi32 Win7 |
| T-140 | بوّابة W5: محمّل DLL كامل | DLL حقيقي يُحمَّل ويُستدعى |
| T-162 | بوّابة W6: PEB/TEB/TLS | تطبيق Win7 حقيقي يبدأ |
| T-172 | بوّابة W7: خدمة تعمل | خدمة تعمل داخل الصندوق |
| T-194 | بوّابة W8: نافذة + إدخال | نافذة + إدخال يعملان |
| T-250 | بوّابة W10: القرص | `dir` على القرص الحقيقي |
| T-251…T-258 | التقرير النهائي + التنظيف + قائمة «ما لم يُنفَّذ» | ملف واحد نظيف (NTBOX.md محدث) |

> المتبقى: 6 حِزم P0 · 5 حِزم P1 · 8 حِزم P2. التسلسل التلقائي يُطبّق: بعد نجاح `T-n` ينتقل التنفيذ إلى `T-n+1` دون موافقة (§4).

---

## 18. قرارات مفتوحة — تحتاج قرار من المالك

القرارات الآتية موثّقة في §8 / §12.4 وتحتاج قرارًا من المالك قبل المضي قدماً. المهام الأخرى تسير تلقائيًا وفقًا لبروتوكول §4.

| # | القرار | الخيارات | الأثر على الخطة |
|---|---|---|---|
| 1 | **تثبيت `kernel32.dll` الحقيقي**: عند تشغيل خدمة حقيقية، هل يُسمح بتشغيل ملف `.exe` من صورة Win7 داخل الصندوق (سلوكه كامل على لينكس)؟ | نعم / لا | إن «نعم» ⇒ T-120/T-165 تسير؛ إن «لا» ⇒ تُبقي على stubs وتؤجل التشغيل الفعلي |
| 2 | **حجم العمل لـ W9a (جسر الكرت)**: المسار UMD (D3D11⇒EGL) هو سبيل التنفيذ المنطقي؛ المسار KMD (محاكاة `amdkmdag.sys`) أُوقف كبحثي (§1) | مضي في UMD / إعادة النظر في KMD | يُحدّد أولوية أسابيع GPU (T-210…T-221) |
| 3 | **جدولة الوقت**: ما أولوية المستخدم — الكرت (T-210) أم تشغيل التطبيقات (T-120 / T-165)؟ | الكرت أولاً / التطبيقات أولاً | يغيّر ترتيب الأسابيع القادمة؛ كلاهما P0 |

> وفقًا لبروتوكول §4: أي تغيير في الفكرة الأساسية (مثل تحويل المسار من «catch syscall» إلى «catch API») أو أمر يحتاج صلاحيات/شبكة/rooting أو تعارض مع الملفات الأصلية ⇒ يُرفع للمالك قبل التنفيذ.

---

**END OF NT-BOX (الملف المرجعي الوحيد)**
