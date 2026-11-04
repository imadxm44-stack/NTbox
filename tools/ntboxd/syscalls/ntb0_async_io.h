/*
 * ntb0_async_io.h — التزامن I/O الحقيقي (T-071..T-073 · SYS-047/SYS-054/SYS-055)
 *
 *   المُرسِل (الاختبار أو ntb0_dispatch لاحقاً) يُسلِّم عقدة (قراءة/كتابة)
 *   مزوّدة بـ: eventfd، ApcRoutine (لويندوز x64)، IOSB، ByteOffset.
 *   worker pthread ينفّذ read/pread — write/pwrite حجمياً ثم:
 *     1. يملأ IO_STATUS_BLOCK (Status + Information = البايتات الفعلية)
 *     2. يُنبِّه eventfd الخاص بالعقدة إن وُجد
 *     3. يعلّم العقدة DONE (قائمة إتمام لكل FileHandle)
 *   جهة العميل تستلم عبر:
 *     - ntb0_async_poll()        (توصيل كل العناصر المكتملة)
 *     - ntb0_async_next_done(H)  (توصيل/سحب عنصر واحد بحسب المقبض)
 *     - ntb0_async_wait_one(H..) (انتظار دون توصيل)
 */
#ifndef NTB0_ASYNC_IO_H
#define NTB0_ASYNC_IO_H
#include <stddef.h>
#include <stdint.h>
#define NTB0_ASYNC_PENDING  0x00000103L
#define NTB0_ST_PENDING     NTB0_ASYNC_PENDING
#define NTB0_ST_CANCELLED   0xC0000120L

typedef enum { NTB0_ASYNC_READ=0, NTB0_ASYNC_WRITE=1 } ntb0_async_type_t;
typedef void (*ntb0_async_cb)(uint64_t handle, int type, long status, size_t bytes_done, void *ctx);
typedef void (*ntb0_async_apc_fn)(void *ctx);

/* مُساعد النداء win64 — معرَّف في ntb0_trampoline.S */
extern long ntb0_call_win64(void *fn, long a1, long a2, long a3, long a4,
                            long a5, long a6, long a7, long a8, long a9,
                            long a10, long a11);

long ntb0_async_submit_read(uint64_t handle, void *buf, size_t len, uint64_t event_handle, ntb0_async_cb cb, void *ctx);
long ntb0_async_submit_write(uint64_t handle, const void *buf, size_t len, uint64_t event_handle, ntb0_async_cb cb, void *ctx);

/*
 * الواجهة الموسّعة (T-057 · T-072 · T-073) — يستهلكها ntb0_dispatch لاحقاً:
 *   event_handle : مقبض eventfd (قد يكون 0)
 *   iosb         : IO_STATUS_BLOCK بذاكرة الكرنك (16بايت، قد يكون NULL)
 *   byte_offset  : -1 = موضع fd الحالي،  وإلا pread64/pwrite64 (T-057)
 *   apc_routine  : IO_APC_ROUTINE (win64 ABI) تُنادى عند الإتمام:
 *                   (rcx=ApcContext, rdx=IoStatusBlock, r8=0)
 *   apc_context  : السياق الممرَّر إلى الـAPC
 */
long ntb0_async_submit_read_ex(uint64_t handle, uint64_t event_handle,
                               void *buf, size_t len,
                               void *iosb, int64_t byte_offset,
                               void *apc_routine, uint64_t apc_context,
                               ntb0_async_cb cb, void *ctx);
long ntb0_async_submit_write_ex(uint64_t handle, uint64_t event_handle,
                                const void *buf, size_t len,
                                void *iosb, int64_t byte_offset,
                                void *apc_routine, uint64_t apc_context,
                                ntb0_async_cb cb, void *ctx);

int ntb0_async_poll(void);
int ntb0_async_wait_one(uint64_t handle, int ms);
void ntb0_async_cancel(uint64_t handle);
void ntb0_async_apc_queue(ntb0_async_apc_fn fn, void *ctx);
int ntb0_async_pending_count(void);
int ntb0_async_next_done(uint64_t handle, long *status, size_t *info);
#endif
