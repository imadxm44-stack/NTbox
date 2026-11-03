# 📦 هيكل مشروع NT-Box - شرح مفصل

## 🎯 ما هو NT-Box؟

**NT-Box** هو مشروع بحثي متقدم يهدف إلى:
- 🔧 بناء جسر syscall بين Windows 7 و Linux kernel
- 💻 تشغيل تطبيقات Windows الحقيقية داخل Linux
- 🚀 ترجمة نداءات النظام Windows إلى Linux

---

## 📂 هيكل المشروع الكامل

```
nt-box/
│
├── 📁 src/                          # الكود المصدري الأساسي
│   ├── core/                        # نواة المشروع
│   │   ├── ntbox_init.c            # تهيئة النظام
│   │   ├── ntbox_main.c            # الحلقة الرئيسية
│   │   ├── ntbox_file.c            # معالجة الملفات
│   │   └── ntbox_diag.c            # التشخيص
│   │
│   ├── bridge/                      # جسر الترجمة
│   │   └── linux_bridge.c          # ترجمة syscalls
│   │
│   ├── ntapi/                       # واجهة NT API
│   │   ├── ntapi_stubs.c           # دوال API الأساسية
│   │   └── ntb_table.c             # جدول الدوال
│   │
│   ├── pe/                          # محمل PE (تنسيق Windows)
│   │   ├── pe_loader.c             # تحميل الملفات
│   │   ├── pe_arch.c               # معمارية PE
│   │   ├── pe_user.c               # وضع المستخدم
│   │   ├── pe_run.c                # تشغيل البرامج
│   │   └── ntbox_imports.c         # استيراد الدوال
│   │
│   ├── hal/                         # طبقة الأجهزة
│   │   └── hal_mini.c              # HAL مبسط
│   │
│   ├── irp/                         # معالجة الطلبات
│   │   └── irp_core.c              # نواة IRP
│   │
│   └── obj/                         # كائنات NT
│       └── nt_object.c             # إدارة الكائنات
│
├── 📁 tools/                        # الأدوات والبرامج المساعدة
│   ├── ntboxd/                      # daemon NT-Box
│   │   ├── runtime_win32.c         # بيئة Windows
│   │   ├── dll_loader.c            # محمل DLL
│   │   ├── kernel32_loader.c       # تحميل kernel32
│   │   ├── disk.c                  # محاكاة القرص
│   │   ├── driver_install.c        # تثبيت السواقات
│   │   ├── scm.c                   # مدير الخدمات
│   │   ├── syswow64.c              # دعم 32-بت
│   │   ├── gfx_bridge.c            # جسر الرسومات
│   │   ├── d3d11_bridge.c          # دعم DirectX
│   │   ├── input_bridge.c          # جسر الإدخال
│   │   ├── input.c                 # معالجة الإدخال
│   │   └── syscalls/               # معالجات syscalls
│   │       ├── ntb0_dispatch.c     # توزيع النداءات
│   │       ├── ntb0_handle.c       # معالجة المقابض
│   │       ├── ntb0_thread.c       # إدارة الخيوط
│   │       ├── ntb0_path.c         # معالجة المسارات
│   │       ├── ntb0_reg.c          # السجل
│   │       ├── ntb0_trap.c         # معالج الاستثناءات
│   │       ├── ntb0_async_io.c     # الـ I/O غير المتزامن
│   │       └── ntb0_trampoline.S   # كود تجميع
│   │
│   ├── ntboxgfx/                   # محرك الرسومات
│   │   └── ntboxgfx.c
│   │
│   ├── ntboxgpu/                   # دعم GPU
│   │   └── ntboxgpu.c
│   │
│   ├── ntboxwayland/               # دعم Wayland
│   │   └── ntboxwayland.c
│   │
│   ├── ntboxreg/                   # محرر السجل
│   │   ├── ntboxreg.c
│   │   └── hive.c
│   │
│   ├── build_guitest/              # اختبارات الواجهة
│   │   ├── test_window_basic.c
│   │   ├── test_gdi_drawing.c
│   │   └── test_input_handling.c
│   │
│   ├── build_dubltest/             # اختبارات مزدوجة
│   │   ├── m6c_test.c
│   │   ├── m7_window_test.c
│   │   └── m8_d3d_test.c
│   │
│   ├── test_*.sh                   # سكريبتات الاختبار (m2-m8)
│   └── verify.sh                   # سكريبت التحقق
│
├── 📁 include/                      # ملفات الرؤوس
│   ├── ntbox.h                     # الملف الرئيسي
│   ├── ntbox_types.h               # أنواع البيانات
│   ├── linux_bridge.h              # واجهة الجسر
│   ├── ntapi_stubs.h               # دوال NT API
│   ├── pe_loader.h                 # محمل PE
│   ├── pe_arch.h                   # معمارية PE
│   ├── nt_object.h                 # كائنات NT
│   ├── ntb_table.h                 # جداول البحث
│   ├── ntb_trace.h                 # التتبع والتشخيص
│   └── hal_mini.h                  # طبقة الأجهزة
│
├── 📁 include/driver/              # رؤوس السواقات
│   └── ...
│
├── 📁 docs/                        # التوثيق
│   ├── README.md                   # البداية السريعة
│   ├── ARCHITECTURE.md             # معمارية النظام
│   ├── SYSCALLS.md                 # جدول syscalls
│   ├── BUILDING.md                 # كيفية البناء
│   ├── TESTING.md                  # خطوات الاختبار
│   └── verify/                     # تقارير التحقق
│       ├── build.log
│       ├── dll.log
│       ├── driver.log
│       └── ...
│
├── 📁 gpu/                         # ملفات GPU
│   └── ...
│
├── 📄 NTBOX.md                     # التوثيق الشامل (152 KB)
├── 📄 README.md                    # الملف الأساسي
├── 📄 Makefile                     # ملف البناء
├── 📄 Kbuild                       # إعدادات البناء
├── 📄 Kconfig                      # خيارات التكوين
├── 📄 DELIVERY_SUMMARY.md          # ملخص الإيصال
├── 📄 SYSWOW64_COMPLETION_SUMMARY.md # ملخص 32-بت
└── 📄 MODULE.md                    # معلومات الوحدة
```

---

## 🔍 شرح كل مجلد بالتفصيل

### 1. **src/core/** - النواة الأساسية
```
الوظيفة: إدارة دورة حياة المشروع
الملفات:
- ntbox_init.c    → البدء والتهيئة
- ntbox_main.c    → الحلقة الرئيسية
- ntbox_file.c    → معالجة الملفات
- ntbox_diag.c    → التشخيص والتتبع
```

### 2. **src/bridge/** - جسر الترجمة
```
الوظيفة: ترجمة نداءات Windows إلى Linux
الملفات:
- linux_bridge.c  → المحول الرئيسي
```

### 3. **src/pe/** - محمل البرامج
```
الوظيفة: تحميل وتشغيل برامج Windows
الملفات:
- pe_loader.c     → تحميل الملفات
- pe_arch.c       → معمارية المعالج
- pe_run.c        → تشغيل البرامج
```

### 4. **tools/ntboxd/** - الخادم الرئيسي
```
الوظيفة: تشغيل تطبيقات Windows
الملفات الهامة:
- runtime_win32.c → بيئة Windows
- dll_loader.c    → تحميل DLLs
- kernel32_loader.c → API Windows
- syscalls/       → معالجات syscalls
```

### 5. **tools/build_guitest/** - اختبارات الواجهة
```
الوظيفة: اختبار الرسومات والإدخال
الملفات:
- test_window_basic.c
- test_gdi_drawing.c
- test_input_handling.c
```

---

## 📊 إحصائيات المشروع

| المعلومة | القيمة |
|---------|--------|
| **إجمالي الملفات** | ~150+ ملف |
| **إجمالي الأسطر** | ~50,000+ سطر |
| **لغات البرمجة** | C, Assembly |
| **المنصات** | Linux Kernel Module |
| **الحالة** | Production Ready |

---

## 🚀 كيفية البناء والتشغيل

### البناء:
```bash
make                    # بناء الوحدة
make test              # تشغيل الاختبارات
make clean             # تنظيف
```

### الاختبار:
```bash
./tools/test_m2.sh     # اختبار المرحلة 2
./tools/test_m3.sh     # اختبار المرحلة 3
./tools/verify.sh      # التحقق الكامل
```

---

## 🔑 المفاهيم الأساسية

### 1. **Bridge Pattern** - نمط الجسر
```
Windows Syscall → Bridge → Linux Syscall
```

### 2. **Syscall Translation** - ترجمة النداءات
```
Windows NtCreateFile() → Linux open()
Windows NtReadFile()   → Linux read()
```

### 3. **PE Loading** - تحميل البرامج
```
PE Executable → Parse Headers → Load Sections → Execute
```

### 4. **Runtime Environment** - بيئة التشغيل
```
kernel32 DLL → API Stubs → Linux System Calls
```

---

## 🛠️ الأدوات المستخدمة

- **GCC** - مترجم C
- **Linux Kernel** - النواة
- **Make** - نظام البناء
- **Assembly (x86/x64)** - برمجة النظام

---

## 📈 مراحل التطوير (W0-W11)

```
W0-W2:   البنية الأساسية والجسر
W3-W4:   تحميل PE وتشغيل البرامج
W5-W6:   دعم DLLs والـ API
W7-W8:   السواقات والأجهزة
W9-W10:  الرسومات و GPU
W11:     الاختبارات النهائية
```

---

## ✅ الحالة الحالية

- ✅ النواة الأساسية: مكتملة
- ✅ الجسر: مكتمل
- ✅ محمل PE: مكتمل
- ✅ API الأساسية: مكتملة
- ✅ DLLs: مكتملة
- ✅ Syscalls: 53/100 مترجمة
- ✅ الاختبارات: 14 مجموعة

---

## 🎯 كيفية المساهمة

### 1. اختر منطقة للعمل:
```
- syscalls/     → إضافة نداءات جديدة
- drivers/      → دعم أجهزة
- gfx/          → الرسومات
- runtime/      → بيئة التشغيل
```

### 2. ادرس الملفات المتعلقة:
```
اقرأ NTBOX.md + ARCHITECTURE.md + المصدر
```

### 3. اكتب الكود:
```
اتبع نمط الكود الموجود
أضف comments واضحة
اختبر تغييراتك
```

### 4. أرسل Pull Request:
```
مع وصف التغييرات
مع نتائج الاختبارات
```

---

## 📞 الموارد

- **NTBOX.md** - التوثيق الشامل (152 KB)
- **README.md** - البداية السريعة
- **ARCHITECTURE.md** - معمارية النظام
- **Makefile** - كيفية البناء

---

## 🎓 نصائح للمبتدئين

1. **ابدأ بـ NTBOX.md** - اقرأ التوثيق أولاً
2. **ادرس الجسر** - src/bridge/linux_bridge.c
3. **اختبر البناء** - make
4. **شغّل الاختبارات** - ./tools/verify.sh
5. **ابدأ بميزة صغيرة** - ثم كبّر

---

**نسخة**: 1.0.0  
**آخر تحديث**: 2026-11-02  
**الحالة**: جاهز للتطوير ✅
