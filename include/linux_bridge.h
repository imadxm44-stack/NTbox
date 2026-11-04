#ifndef LINUX_BRIDGE_H
#define LINUX_BRIDGE_H

#include <linux/types.h>
#include <linux/string.h>

#define NTBOX_LOG(fmt, ...) pr_info("[ntbox] " fmt, ##__VA_ARGS__)
#define NTBOX_ERR(fmt, ...) pr_err("[ntbox] ERR: " fmt, ##__VA_ARGS__)

typedef enum irqreturn (*NTB_IRQ_HANDLER)(int irq, void *dev);

/* ---------- M1: memory / IRQ bridges ---------- */

/* Memory - المرحلة الأولى للديمو */
void *ntb_kmalloc(size_t size, gfp_t flags);
void ntb_kfree(void *ptr);

/* I/O Memory - إطار للمستقبل، لا يستخدم في Skeleton */
int NTB_MSABI ntb_ioremap(phys_addr_t phys_addr, unsigned long size,
                          void __iomem **addr_out);void ntb_iounmap(void __iomem *addr);

/* IRQ - إطار للمستقبل */
int NTB_MSABI ntb_request_irq(unsigned int irq, NTB_IRQ_HANDLER handler,
                      unsigned long flags, const char *name, void *dev);void ntb_free_irq(unsigned int irq, void *dev);

/* ---------- M2: وصول البرتشنز (كل شيء عبر الجسر — لا عتاد مباشر) ---------- */

struct ntb_partition {
    char name[32];
    unsigned long major;
    unsigned long minor;
    unsigned long long blocks;
};

bool ntb_dev_exists(const char *dev);
int  ntb_partitions_scan(struct ntb_partition *out, int max);
int  ntb_file_open_read(const char *path, char **buf_out, size_t *size_out);
void ntb_file_close_buf(char *buf);

/* ---------- M9: GPU / كرت الشاشة — جسر حقيقي إلى DRM/amdgpu (T-209..T-212) ----------
 * كل أوامر الكرت من ويندوز تترجم هنا إلى ioctls لينكس حقيقية على عقدة الرسم
 * (/dev/dri/renderD128) — لا نجاح مزيّف: كل فشل يُرجع errno خالياً + مصباح.
 * العقد (أرقام ثابتة للـ UAPI değerند + نفسها في tools/ntboxgpu).            */

/* عقد الرسم المفتوح = مقبض تتالو صغير (1..NTB_GPU_HANDLE_MAX). (T-209:
 * السابق كان يقطع مؤشر Struct file إلى int — بعد الإلغاء (64→32 بت)
 * يُفقَد النصف العلوي ⇒ إعادة بناء المؤشر تعطي مؤشراً مفسّداً ⇒ crash). */
#define NTB_GPU_HANDLE_MAX 16

/* مساحة العناوين الظاهرية للكرة — كل BO يُرسَمَه الجسر يبدأ من هنا (4 GiB).
 * منطقة عالية آمنة داخل GPUVM (بدأ دليل VA المحجوز هو 2 MiB فقط عند 0). */
#define NTB_GPU_VA_BASE 0x0000000100000000ULL
#define NTB_GPU_CS_MAX_IB 8

/* وصف IB (instruction buffer) — يُحدَّد بالذاكرة التي رسمها ntb_gpu_va_map */
struct ntb_gpu_ib {
    u64 va;      /* عنوان GPU ظاهري (يجب أن يكون EXECUTABLE map) */
    u32 dw;      /* الحجم بالكلمات (u32) — > 0 */
    u32 ring;    /* رقم الحلقة داخل GFX IP (0 = الحلقة الافتراضية) */
};

/* كط أمر مُرسَل إلى ntb_gpu_submit_cmd() (تعويض T-210 — واقعي على amdgpu):
 * الجسر يبني تراكيب amdgpu CS (bo list + chunks + WAIT fence) داخلياً؛
 * كل مقبض BO مطلوب لIB المُشار إليه يُعاد من جدول VA-map الخاص بالجسر. */
struct ntb_gpu_cmd {
    u32 ctx;      /* 0 = استخدم سياق الجسر الخاص (ينشأ تلقائياً) */
    u32 n_ib;     /* عدد الـ IBs (1..NTB_GPU_CS_MAX_IB) */
    u32 n_bo;     /* عدد مقابض BO (0 = الجسر يبني من جدول VA الخاص به) */
    const u32 *bo;  /* المقابض (مؤشر نواة) */
    const struct ntb_gpu_ib *ib;  /* أوصاف الـ IBs (مؤشر نواة) */
};

/* عقدة الرسم */
int  NTB_MSABI ntb_gpu_open_render(const char *devnode);    /* → مقبض ≥ 1 */
void NTB_MSABI ntb_gpu_close_render(int h);
int  NTB_MSABI ntb_gpu_create_gem(int h, size_t size, u32 *handle);
int  NTB_MSABI ntb_gpu_mmap_gem(int h, u32 handle, size_t size, void **addr);
int  NTB_MSABI ntb_gpu_unmap_gem(int h, void *addr);
int  NTB_MSABI ntb_gpu_get_dmabuf(int h, u32 handle, int *dmabuf_fd);

/* مسار الإرسال الحقيقي (T-210) — أصول amdgpu CS */
int  NTB_MSABI ntb_gpu_ctx_create(int h, u32 *ctx);          /* GPU-010 */
int  NTB_MSABI ntb_gpu_bo_list_create(int h, const u32 *handles, u32 n, u32 *list); /* GPU-011 */
int  NTB_MSABI ntb_gpu_bo_list_destroy(int h, u32 list);
int  NTB_MSABI ntb_gpu_va_map(int h, u32 bo, u64 bytes, u64 *va);      /* GPU-012 */
int  NTB_MSABI ntb_gpu_va_unmap(int h, u64 va);
int  NTB_MSABI ntb_gpu_submit_cmd(int h, const void *cmd, size_t cmd_size); /* GPU-006 */
int  NTB_MSABI ntb_gpu_wait_cmd(int h, u64 seq);           /* GPU-013 (seq=0 ⇒ آخر إرسال) */

/* العرض والانقلاب (T-211) والاستعلام (T-212) */
int  NTB_MSABI ntb_gpu_present(int h, u32 handle, u32 width, u32 height);   /* GPU-007 */
int  NTB_MSABI ntb_gpu_query_adapter(int h, char *info, size_t infolen);   /* GPU-008 */

/* تفريغ كل مقابض العقد المفتوحة — يُستدعى عند تفريغ الوحدة */
void ntb_gpu_exit(void);

/* ---------- نتائج اختبار ذاتي (ntb_table.c) ----------
 * T_OK=1 نجح · T_FAIL=0 فشل · T_NONE=-1 لا اختبار/تخطّى — تُعرَّف محليّاً
 * في الملف المنادي لـ ntb_table_run_test() (انظر /proc/ntbox/selftest).
 */
#endif
