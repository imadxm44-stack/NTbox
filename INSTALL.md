# دليل التثبيت - NT-Box

دليل شامل لتثبيت NT-Box على أنظمة Linux المختلفة.

---

## 📋 المتطلبات الأساسية

### البرامج المطلوبة:
- **GCC** أو **Clang** (الإصدار 9+)
- **Make** و **CMake** (الإصدار 3.10+)
- **Linux Headers** لنواة النظام الحالية
- **Git** (لاستنساخ المشروع)
- **Python 3.6+** (اختياري، للأدوات)

### متطلبات النظام:
- **ذاكرة RAM:** 2 GB على الأقل
- **مساحة تخزين:** 500 MB (للبناء الكامل)
- **صلاحيات:** الوصول إلى root (للتثبيت في /usr/local)

---

## 🐧 التثبيت حسب التوزيعة

### Ubuntu / Debian

```bash
# تحديث قوائم الحزم
sudo apt-get update

# تثبيت المتطلبات الأساسية
sudo apt-get install -y \
    build-essential \
    linux-headers-$(uname -r) \
    cmake \
    git \
    make

# تثبيت المتطلبات الإضافية (اختيارية)
sudo apt-get install -y \
    clang \
    llvm \
    valgrind \
    gdb \
    python3-dev
```

### Fedora / RHEL / CentOS

```bash
# تثبيت المتطلبات الأساسية
sudo dnf install -y \
    gcc \
    gcc-c++ \
    make \
    cmake \
    git \
    kernel-devel

# تثبيت المتطلبات الإضافية (اختيارية)
sudo dnf install -y \
    clang \
    llvm-devel \
    valgrind \
    gdb \
    python3-devel
```

### Arch Linux

```bash
# تثبيت المتطلبات الأساسية
sudo pacman -S \
    base-devel \
    linux-headers \
    cmake \
    git \
    make

# تثبيت المتطلبات الإضافية (اختيارية)
sudo pacman -S \
    clang \
    llvm \
    valgrind \
    gdb
```

### openSUSE

```bash
# تثبيت المتطلبات الأساسية
sudo zypper install -y \
    gcc \
    make \
    cmake \
    git \
    kernel-devel

# تثبيت المتطلبات الإضافية (اختيارية)
sudo zypper install -y \
    clang \
    llvm-devel \
    valgrind \
    gdb
```

---

## 🚀 خطوات التثبيت

### 1. استنساخ المشروع

```bash
# استنساخ من GitHub
git clone https://github.com/IMADIX44/NT-Box.git
cd NT-Box

# أو باستخدام SSH
git clone git@github.com:IMADIX44/NT-Box.git
cd NT-Box
```

### 2. التحقق من المتطلبات

```bash
# التحقق من وجود المترجم
gcc --version
# أو
clang --version

# التحقق من Linux Headers
uname -r
ls /usr/src/linux-headers-$(uname -r) 2>/dev/null || echo "Headers not found"

# التحقق من Make و CMake
make --version
cmake --version
```

### 3. البناء

#### الطريقة 1: استخدام Makefile (الموصى به)

```bash
# تنظيف البناء السابق
make clean

# بناء بخيط واحد
make

# بناء بخيوط متعددة (أسرع)
make -j$(nproc)

# بناء مع معالجة أخطاء مفصلة
make V=1
```

#### الطريقة 2: استخدام CMake

```bash
# إنشاء مجلد البناء
mkdir build
cd build

# إنشاء ملفات البناء
cmake ..

# البناء
make -j$(nproc)

# البناء مع تحسينات الأداء
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 4. التثبيت

#### التثبيت في النظام (يتطلب root)

```bash
# التثبيت الكامل
sudo make install

# أو باستخدام CMake
sudo make -C build install
```

#### التثبيت في مجلد مخصص (بدون root)

```bash
# تحديد مسار التثبيت
make install PREFIX=$HOME/ntbox-install

# أو باستخدام CMake
cmake .. -DCMAKE_INSTALL_PREFIX=$HOME/ntbox-install
make install
```

### 5. التحقق من التثبيت

```bash
# فحص ملفات التثبيت
ls -la /usr/local/bin/ntshell
ls -la /usr/local/lib/libntbox*

# أو إذا ثبتت في مجلد مخصص
ls -la $HOME/ntbox-install/bin/
ls -la $HOME/ntbox-install/lib/

# التحقق من وحدة Kernel
lsmod | grep ntbox

# تشغيل الأداة الرئيسية
ntshell --version
```

---

## 🧪 اختبار التثبيت

```bash
# تشغيل الاختبارات الأساسية
make test

# تشغيل الاختبارات مع المخرجات المفصلة
make test V=1

# اختبار محدد
make test-core
make test-bridge
make test-hal

# اختبار الأداء
make benchmark
```

---

## 🔧 خيارات البناء المتقدمة

### تجميع بطريقة محسنة

```bash
# بناء مع تحسينات الأداء
CFLAGS="-O3 -march=native" make -j$(nproc)

# بناء مع معلومات التصحيح
CFLAGS="-g -O0" make -j$(nproc)

# بناء مع كل التحذيرات
CFLAGS="-Wall -Wextra -Wpedantic -O2" make -j$(nproc)
```

### خيارات CMake

```bash
# بناء Release (محسّن)
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_OPTIMIZATION_LEVEL=3

# بناء Debug (للتصحيح)
cmake .. -DCMAKE_BUILD_TYPE=Debug

# تعطيل ميزات معينة
cmake .. -DENABLE_GPU=OFF -DENABLE_WAYLAND=OFF

# تفعيل ميزات إضافية
cmake .. -DENABLE_TESTS=ON -DENABLE_BENCHMARKS=ON
```

---

## 🧹 إزالة التثبيت

### إذا ثبت في النظام

```bash
# إزالة الملفات المثبتة
sudo make uninstall

# أو يدويًا
sudo rm -f /usr/local/bin/ntshell
sudo rm -f /usr/local/bin/ntboxd
sudo rm -f /usr/local/lib/libntbox*
sudo rm -rf /usr/local/include/ntbox
sudo rm -rf /usr/local/share/ntbox
```

### إذا ثبت في مجلد مخصص

```bash
# ببساطة احذف المجلد
rm -rf $HOME/ntbox-install
```

### حذف المشروع المحلي

```bash
# الرجوع إلى الديرتوري الأب
cd ..

# حذف المجلد
rm -rf NT-Box
```

---

## 🐛 حل المشاكل الشائعة

### المشكلة: "linux/kernel.h: No such file or directory"

**الحل:**
```bash
# تثبيت Linux Headers
sudo apt-get install linux-headers-$(uname -r)    # Ubuntu/Debian
sudo dnf install kernel-devel                     # Fedora
sudo pacman -S linux-headers                      # Arch

# أو تحديد مسار Headers يدويًا
make KDIR=/usr/src/linux-headers-$(uname -r)
```

### المشكلة: "gcc: command not found"

**الحل:**
```bash
# تثبيت build-essential
sudo apt-get install build-essential              # Ubuntu/Debian
sudo dnf install gcc make                         # Fedora
sudo pacman -S base-devel                         # Arch
```

### المشكلة: "Permission denied" عند التثبيت

**الحل:**
```bash
# استخدم sudo
sudo make install

# أو ثبت في مجلد مخصص بدون root
make install PREFIX=$HOME/ntbox-install

# أو غيّر الصلاحيات
sudo chown -R $USER /usr/local/
make install
```

### المشكلة: "CMake version is less than required"

**الحل:**
```bash
# تحديث CMake
sudo apt-get upgrade cmake                        # Ubuntu/Debian
sudo dnf upgrade cmake                            # Fedora

# أو استخدم Makefile بدلاً منه
make -j$(nproc)
```

### المشكلة: "No such file or directory" في البناء

**الحل:**
```bash
# تنظيف البناء السابق
make clean

# إعادة البناء من البداية
make -j$(nproc)

# إذا استمرت المشكلة، استخدم البناء الفردي
make V=1
```

---

## 📚 الخطوات التالية

بعد التثبيت الناجح:

1. **اقرأ التوثيق:**
   ```bash
   cat docs/ARCHITECTURE_OVERVIEW.md
   cat docs/BUILD_GUIDE.md
   ```

2. **جرّب الأدوات:**
   ```bash
   ntshell --help
   ntboxd --version
   peimports --help
   ```

3. **شغّل الاختبارات:**
   ```bash
   make test
   ```

4. **اقرأ دليل المطورين:**
   ```bash
   cat CONTRIBUTING.md
   ```

---

## 🆘 الحصول على الدعم

إذا واجهت مشكلة:

1. **فحص السجلات:**
   ```bash
   dmesg | tail -20          # سجل kernel
   journalctl -n 50          # سجل النظام
   ```

2. **ابحث عن الحل:**
   - [GitHub Issues](https://github.com/IMADIX44/NT-Box/issues)
   - [GitHub Discussions](https://github.com/IMADIX44/NT-Box/discussions)

3. **أنشئ issue جديد:**
   - اذكر التوزيعة والإصدار
   - أرفق رسالة الخطأ كاملة
   - اشرح خطوات إعادة الإنتاج

---

## ✅ قائمة التحقق

- [ ] تثبيت جميع المتطلبات
- [ ] استنساخ المشروع
- [ ] تشغيل `make clean`
- [ ] تشغيل `make -j$(nproc)`
- [ ] تشغيل `make test`
- [ ] تشغيل `sudo make install`
- [ ] التحقق من الأدوات (`ntshell --version`)

---

**تم التحديث:** نوفمبر 2026  
**الحالة:** ✅ جاهز للاستخدام
