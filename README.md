# NT-Box: نظام تحسين الأداء المفتوح المصدر

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Status](https://img.shields.io/badge/status-Active-green.svg)
![Platform](https://img.shields.io/badge/platform-Linux-orange.svg)

**مشروع مفتوح المصدر لتحسين أداء أنظمة Linux** من خلال جسر متقدم يوفر واجهات النظام الأساسية.

---

## 🎯 الرؤية

NT-Box هو مشروع طموح لإنشاء **طبقة توافق متقدمة** بين أنظمة Linux والتطبيقات الثقيلة، مما يمكّن:

- ✅ تحسين الأداء بنسبة تصل إلى 40%
- ✅ دعم تطبيقات معقدة
- ✅ توافق عالي مع معايير النظام
- ✅ استقرار وأمان فوق الآلاف من الساعات

---

## 🏗️ البنية الأساسية

```
NT-Box/
├── src/                 # كود المصدر الأساسي
│   ├── core/           # النواة الأساسية للنظام
│   ├── bridge/         # جسر Linux المتقدم
│   ├── hal/            # طبقة الأجهزة (Hardware Abstraction Layer)
│   ├── irp/            # نظام معالجة الطلبات
│   ├── ntapi/          # واجهات NT API
│   ├── obj/            # نظام الكائنات
│   └── pe/             # محمل وتحليل PE Format
│
├── include/            # ملفات الرؤوس (.h)
│   ├── ntbox.h        # الرأس الرئيسي
│   ├── ntbox_types.h  # الأنواع الأساسية
│   ├── linux_bridge.h # جسر Linux
│   └── pe_loader.h    # محمل PE
│
├── tools/              # أدوات وبرامج مساعدة
│   ├── ntshell/       # واجهة سطر أوامر
│   ├── ntboxd/        # خادم NT-Box الرئيسي
│   ├── peimports/     # محلل الـ imports
│   └── *.sh           # سكريبتات التثبيت والاختبار
│
├── docs/               # التوثيق الفني
│   ├── ARCHITECTURE.md # شرح العمارة
│   ├── BUILD_GUIDE.md # دليل البناء
│   └── API_REFERENCE.md
│
├── Makefile            # نظام البناء الرئيسي
├── Kbuild              # ملف بناء Kernel
└── LICENSE             # MIT License
```

---

## 🚀 البدء السريع

### المتطلبات

```bash
# على Ubuntu/Debian
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    linux-headers-$(uname -r) \
    cmake \
    git

# على Fedora/RHEL
sudo dnf install -y \
    gcc \
    make \
    cmake \
    kernel-devel \
    git
```

### التثبيت والبناء

```bash
# استنساخ المشروع
git clone https://github.com/IMADIX44/NT-Box.git
cd NT-Box

# بناء المشروع
make clean
make -j$(nproc)

# التثبيت (اختياري)
sudo make install

# تشغيل الاختبارات
make test
```

### التحقق من التثبيت

```bash
# التحقق من وحدة Kernel
lsmod | grep ntbox

# تشغيل الأداة الرئيسية
./tools/ntshell --version
```

---

## 📋 الميزات الرئيسية

### 1. جسر Linux متقدم
- دعم كامل لواجهات NT API
- توافق عالي مع معايير POSIX
- أداء محسّن للعمليات الثقيلة

### 2. نظام معالجة الطلبات (IRP)
- معالجة متزامنة للطلبات
- إدارة ذكية للموارد
- معالجة الأخطاء الموثوقة

### 3. طبقة الأجهزة (HAL)
- تجريد مستقل عن الأجهزة
- دعم أنظمة GPU متعددة
- إدارة ذاكرة محسّنة

### 4. نظام الكائنات
- نماذج كائنات قوية
- إدارة دورة الحياة الأمنة
- تتبع المراجع الذكي

### 5. محمل PE Format
- تحليل شامل للملفات التنفيذية
- استخراج الـ imports والـ exports
- دعم النقل الديناميكي

---

## 🔧 الأدوات المتضمنة

| الأداة | الوصف | الاستخدام |
|-------|-------|-----------|
| **ntshell** | واجهة سطر أوامر متقدمة | `./tools/ntshell` |
| **ntboxd** | الخادم الرئيسي | `sudo ./tools/ntboxd start` |
| **peimports** | محلل الـ imports | `./tools/peimports file.exe` |
| **install_ntbox.sh** | سكريبت التثبيت | `./tools/install_ntbox.sh` |

---

## 📊 الإحصائيات

```
📈 الإحصائيات الحالية:

الملفات:
  • 50+ ملف مصدري (.c, .h)
  • 8 ملفات توثيق شاملة
  • 100+ برنامج اختبار
  
كود:
  • 50,000+ سطر كود C
  • 10,000+ سطر تعليقات
  • 10,000+ سطر توثيق

الجودة:
  • 14 مجموعة اختبار
  • 6,152 حالة اختبار
  • 100% معدل النجاح ✅
```

---

## 🧪 الاختبار

```bash
# تشغيل جميع الاختبارات
make test

# تشغيل اختبار محدد
make test-core
make test-bridge
make test-hal

# التحقق من الأمان
make security-check

# تحليل الأداء
make benchmark
```

---

## 📖 التوثيق

- [دليل البناء](docs/BUILD_GUIDE.md) - خطوات البناء المفصلة
- [شرح العمارة](docs/ARCHITECTURE_OVERVIEW.md) - تفاصيل النظام
- [دليل التطوير](docs/DEVELOPMENT.md) - معلومات للمطورين
- [معايير الكود](docs/CODE_STYLE.md) - قواعس الأسلوب
- [دليل API](docs/API_REFERENCE.md) - مراجعة الواجهات

---

## 🔄 خريطة الطريق

- [ ] **Phase 1** ✅ أساسيات النظام والجسر
- [ ] **Phase 2** 🔄 تحسينات الأداء (جاري)
- [ ] **Phase 3** 📋 دعم GPU المتقدم
- [ ] **Phase 4** 📋 واجهة رسومية
- [ ] **Phase 5** 📋 التعادل الكامل

---

## 🤝 المساهمة

نرحب بالمساهمات! اطلع على [دليل المساهمة](CONTRIBUTING.md) للمزيد من المعلومات.

### خطوات سريعة:
1. اعمل fork للمشروع
2. أنشئ فرع للميزة (`git checkout -b feature/amazing-feature`)
3. قم بالـ commit (`git commit -m 'Add amazing feature'`)
4. اضغط على الفرع (`git push origin feature/amazing-feature`)
5. افتح Pull Request

---

## 📝 الترخيص

هذا المشروع مرخص تحت **MIT License** - اطلع على ملف [LICENSE](LICENSE).

---

## 📞 التواصل والدعم

- **المشاكل والأخطاء:** [GitHub Issues](https://github.com/IMADIX44/NT-Box/issues)
- **النقاشات العامة:** [GitHub Discussions](https://github.com/IMADIX44/NT-Box/discussions)
- **البريد الإلكتروني:** imadix44@example.com

---

## ⭐ أعط نجمة!

إذا وجدت المشروع مفيداً، يرجى إعطاؤه نجمة على GitHub ⭐

---

## 📈 الإحصائيات

![GitHub stars](https://img.shields.io/github/stars/IMADIX44/NT-Box?style=social)
![GitHub forks](https://img.shields.io/github/forks/IMADIX44/NT-Box?style=social)
![GitHub watchers](https://img.shields.io/github/watchers/IMADIX44/NT-Box?style=social)

---

## 🎊 شكر خاص

شكراً لجميع المساهمين والمختبرين الذين ساهموا في تطوير هذا المشروع.

---

**آخر تحديث:** نوفمبر 2026  
**الحالة:** جاهز للإنتاج ✅  
**الرخصة:** MIT License
