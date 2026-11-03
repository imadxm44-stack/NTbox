# 🚀 دليل البدء السريع - NT-Box

## مرحباً! 👋

أنت الآن تملك **NT-Box v1.0.0** - مشروع بحثي متقدم لتشغيل Windows داخل Linux!

---

## 📌 ما هو NT-Box؟ (في 30 ثانية)

```
Windows 7 Program
      ↓
  NT-Box Bridge
      ↓
Linux Kernel
```

**الفكرة:** تشغيل برامج Windows الحقيقية على Linux عن طريق ترجمة نداءات النظام!

---

## 🏃 البدء السريع (5 دقائق)

### 1. تثبيت المتطلبات

```bash
# على Linux
sudo apt-get install build-essential linux-headers

# تحقق من التثبيت
gcc --version
make --version
```

### 2. البناء

```bash
cd nt-box
make                    # بناء الوحدة
make test              # اختبار
```

### 3. التشغيل

```bash
./tools/verify.sh      # التحقق من النظام
```

---

## 📚 الملفات المهمة

| الملف | الوصف |
|------|-------|
| **NTBOX.md** | التوثيق الشامل (اقرأه!) |
| **PROJECT_STRUCTURE.md** | شرح هيكل المشروع |
| **Makefile** | كيفية البناء |
| **README.md** | معلومات إضافية |

---

## 🎯 الخطوات التالية

### للمبتدئين:
```
1. اقرأ: NTBOX.md (معمارية المشروع)
2. ادرس: src/bridge/ (جسر الترجمة)
3. جرّب: make test
```

### للمطورين:
```
1. افتح: src/core/ntbox_main.c
2. ادرس: tools/ntboxd/ (الخادم الرئيسي)
3. أضف: ميزات جديدة
4. اختبر: ./tools/test_*.sh
```

### للمساهمين:
```
1. اختر ملف للعمل عليه
2. ادرس التعليقات والكود
3. اكتب التحسينات
4. أرسل Pull Request
```

---

## 🔧 الأوامر الأساسية

```bash
make              # بناء المشروع
make test         # اختبارات سريعة
make verify       # تحقق شامل
make clean        # تنظيف الملفات المؤقتة
make distclean    # حذف كل شيء

./tools/test_m2.sh    # اختبار المرحلة 2
./tools/test_m3.sh    # اختبار المرحلة 3
./tools/verify.sh     # التحقق من كل شيء
```

---

## 🐛 استكشاف الأخطاء

### الخطأ: "make: command not found"
```bash
sudo apt-get install build-essential
```

### الخطأ: "linux/kernel.h: No such file"
```bash
sudo apt-get install linux-headers-$(uname -r)
```

### الخطأ: "Permission denied"
```bash
chmod +x tools/test_*.sh
chmod +x tools/verify.sh
```

---

## 📊 معلومات سريعة

```
الحجم:              ~20 MB
الملفات:            ~150+ ملف
أسطر الكود:         ~50,000+
الدعم:              Linux x86/x64
النسخة:             1.0.0
الحالة:             جاهز للتطوير ✅
```

---

## 🔗 الموارد

- **NTBOX.md** - التوثيق الكامل
- **PROJECT_STRUCTURE.md** - شرح الهيكل
- **src/** - الكود المصدري
- **tools/** - الأدوات والاختبارات
- **include/** - ملفات الرؤوس

---

## 💬 نصائح مهمة

✅ ابدأ بـ **NTBOX.md**  
✅ ادرس **src/bridge/** أولاً  
✅ جرّب **make test**  
✅ اقرأ التعليقات في الكود  
✅ لا تخاف من الأسئلة  

---

## 🎓 الخطوة التالية

```
اختر واحد:

1. اقرأ NTBOX.md
   ↓
2. ادرس src/bridge/linux_bridge.c
   ↓
3. جرّب make test
   ↓
4. ابدأ بتحسينات صغيرة
```

---

**استمتع بـ NT-Box! 🚀**

آخر تحديث: 2026-11-02
