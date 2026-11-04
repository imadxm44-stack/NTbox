KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

# مسار جذر Windows 7 المستخرج:
#   /tmp/opencode/win7root أولاً (سريع)، ثم النسخة الدائمة التي يبنيها
#   tools/prepare_win7.sh (تنجو من إعادة تشغيل الجهاز ⇒ /tmp يُمسح).
WIN7 ?= $(firstword $(wildcard /tmp/opencode/win7root win7root win7full))

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

load:
	sudo insmod ntbox.ko

unload:
	sudo rmmod ntbox

logs:
	dmesg | tail -n 30

reload: unload load logs

# ── أدوات البيئة (NT-Box Environment) ──────────────────────────────

# مستخرج جدول syscall من ntdll.dll الحقيقي
ntb0extract:
	gcc -O2 -Wall -o tools/ntboxreg/ntb0extract tools/ntboxreg/ntb0extract.c

# إعادة توليد جدول الـ syscalls من ntdll الحقيقي (لا تُعدّله يدوياً)
syscall-table: ntb0extract
	./tools/ntboxreg/ntb0extract $(WIN7)/Windows/System32/ntdll.dll \
	    --emit-c tools/ntboxreg/ntdll_syscalls.c \
	    --emit-h tools/ntboxreg/ntdll_syscalls.h

# بناء اختبار مصيدة syscalls
test_trap:
	cd tools/ntboxd/syscalls && gcc -O2 -Wall -Wextra -pthread -I../../ntboxreg -I. \
	    -o test_trap test_trap.c ../../ntboxreg/ntdll_syscalls.c \
	    ntb0_trap.c ntb0_dispatch.c ntb0_path.c ntb0_handle.c ntb0_thread.c \
	    ntb0_reg.c ntb0_async_io.c \
	    ntb0_trampoline.S

# الاختبار الذاتي لطبقة الترجمة (يجب أن يمرّ)
selftest: test_trap
	./tools/ntboxd/syscalls/test_trap $(WIN7)/Windows/System32/ntdll.dll $(WIN7)

# ── نظام الاختبار الموحّد (Workstream 0) ──────────────────────────────
# تقرير واحد بصيغة ثابتة: docs/verify_report.txt
# VERDICT: PASS ⇔ كل المجموعات ناجحة وصفر مصباح FAIL
verify:
	./tools/verify.sh

# نفس contractual لكن بلا مجموعة النواة (لا يحتاج root)
verify-quick:
	./tools/verify.sh --quick

# عرض آخر تقرير
report:
	@cat docs/verify_report.txt

.PHONY: all clean load unload logs reload ntb0extract syscall-table test_trap selftest verify verify-quick report


# NT-Box GUI build (GTK3)
GUI_DIR := tools/ntbox-gui
GUI_BIN := $(GUI_DIR)/ntbox-gui

.PHONY: gui gui-clean

gui:
	@if [ -f $(GUI_DIR)/Makefile ]; then $(MAKE) -C $(GUI_DIR); else \
	echo "Building ntbox-gui (C/GTK3)..."; \
	gcc -O2 -Wall $$(pkg-config --cflags gtk+-3.0 2>/dev/null || echo "-I/usr/include/gtk-3.0 -I/usr/include/at-spi2-atk/2.0 -I/usr/include/at-spi-2.0 -I/usr/include/dbus-1.0 -I/usr/lib/x86_64-linux-gnu/dbus-1.0/include -I/usr/include/gtk-3.0 -I/usr/include/gio-unix-2.0 -I/usr/include/cairo -I/usr/include/pango-1.0 -I/usr/include/fribidi -I/usr/include/harfbuzz -I/usr/include/atk-1.0 -I/usr/include/cairo -I/usr/include/pixman-1 -I/usr/include/uuid -I/usr/include/freetype2 -I/usr/include/libpng16 -I/usr/include/gdk-pixbuf-2.0 -I/usr/include/libmount -I/usr/include/blkid -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include") \
	-o $(GUI_BIN) $(GUI_DIR).c $$(pkg-config --libs gtk+-3.0 2>/dev/null || echo "-lgtk-3 -lgdk-3 -lpangocairo-1.0 -lpango-1.0 -latk-1.0 -lcairo-gobject -lcairo -lgdk_pixbuf-2.0 -lgio-2.0 -lgobject-2.0 -lglib-2.0") 2>&1 | tail -5; fi

gui-clean:
	rm -f $(GUI_BIN) $(GUI_DIR)/*.o

# ── NT-Box GUI ──────────────────────────────────────────────────────
GUI_BIN := tools/ntbox-gui
.PHONY: gui gui-build gui-clean

gui: gui-build
	@echo "NT-Box GUI built at $(GUI_BIN)"

gui-build:
	@chmod +x $(GUI_BIN)
	@echo "ntbox-gui ready (Python GTK3)"

gui-clean:
	@echo "GUI build files cleaned"
