/*
 * ntb0_trampoline.S — قفزة الاعتراض (trampoline) لستوب syscall في ntdll.
 *
 * ═══════════════════════════════════════════════════════════════════════
 * لماذا هذا الملف (PLAN_NTB0_ENV.md §4.2 — مُقاس ومُتحقَّق):
 *
 *   ntdll.dll الحقيقي من Windows 7 x64 يستدعي النواة مباشرة:
 *
 *       4C 8B D1        mov  r10, rcx        ; arg1  (اصطلاح ويندوز)
 *       B8 <num32>      mov  eax, <num>      ; رقم syscall
 *       0F 05           syscall              ; ← نقطة الاعتراض
 *       C3              ret
 *
 *   هذا يعني: الترجمة تقع عند الـ syscall لا عند كل API.
 *   الـ DLLs تعمل مباشرة؛ نحن نلتقط الرقم فقط.
 *
 *   نغيّر البايتات 0F 05 (و ما بعدها) إلى:
 *
 *       E9 <rel32>      jmp  <trampoline>    ; 5 بايت
 *       90 90 90        nop nop nop          ; 3 بايت حشو
 *
 *   القيود المقاسة على الملف الحقيقي:
 *     - كل stub في ntdll يأخذ خانة 16 بايت (394 من 398) أو 32 (3)
 *     - البايتات 8..15 من كل خانة حرة (syscall + ret + حشو nop)
 *     - المسافة بين صورة ntdll والقفزة يجب أن تكون < 2GB (rel32)
 *       → لذلك نحط الصورة والـ trampoline في نفس mmap واحد.
 *
 *   تحذير مُقاس: syscall برقم ويندوز (مثل 0x52) يُنفَّذ كـ syscall لينكس
 *   (0x52 = rename())! لذلك الاعتراض يجب أن يقع **قبل** تنفيذ syscall
 *   — ولا يمكن الاعتماد على SIGSEGV.
 * ═══════════════════════════════════════════════════════════════════════
 *
 * حالة الدخول (اصطلاح ويندوز x64، بعد أن نفّذ الـ stub mov r10,rcx/mov eax,N):
 *
 *     rax          = رقم syscall
 *     r10          = arg1
 *     rdx          = arg2
 *     r8           = arg3
 *     r9           = arg4
 *     [rsp+0x28]   = arg5      ← في Win64 يُمرَّر على المكدس وليس في سجل
 *     [rsp+0x30]   = arg6
 *     [rsp+0x38]   = arg7
 *     [rsp+0x40]   = arg8
 *     [rsp+0x48]   = arg9
 *     [rsp+0x50]   = arg10
 *     [rsp+0x58]   = arg11
 *     [rsp]        = عنوان العودة إلى كود ويندوز
 *
 * الهدف (اصطلاح SysV):
 *
 *     long ntb0_dispatch(long num, long a1, long a2, long a3, long a4,
 *                        long a5, long a6, long a7, long a8, long a9,
 *                        long a10, long a11)
 *
 *     rdi=num  rsi=a1  rdx=a2  rcx=a3  r8=a4  r9=a5
 *     a6..a11 على المكدس: [rsp+0x00]=a6 .. [rsp+0x28]=a11 عند النداء
 *     (اصطلاح SysV: المعامل السابع فما فوق على المكدس)
 *
 * ملاحظة مهمة: rdi و rsi **غير متغيّرين** في اصطلاح ويندوز (callee-saved)
 * بينما هما متغيّران في SysV — لذلك نحفظهما ونعيدهما.
 */

    .text
    .globl ntb_trampoline_template
    .type ntb_trampoline_template,@function
ntb_trampoline_template:
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    push %rdi            /* rdi غير متغيّر في Win64 */
    push %rsi            /* rsi غير متغيّر في Win64 */
    sub  $8, %rsp        /* حشو المحاذاة (16-بايت) + موضع المعاملات */

    /*
     * المعاملات على مكدس ويندوز:
     *   دفعنا 8 سجلات (64 بايت) + 8 بايت حشو = 0x48
     *   arg5  = [rsp_entry+0x28] → 0x28 + 0x48 = 0x70
     *   arg6  = [rsp_entry+0x30] → 0x78
     *   ...   arg11 = [rsp_entry+0x58] → 0xA0
     */
    mov  %eax, %edi       /* رقم syscall */
    mov  %r10, %rsi       /* arg1 */
    mov  %r8,  %rcx       /* arg3 */
    mov  %r9,  %r8        /* arg4 */
    mov  0x70(%rsp), %r11 /* arg5 */
    mov  %r11, %r9        /* arg5 */

    /*
     * تمرير arg6..arg11 إلى المترجم (SysV: المعامل 7 فما فوق على المكدس).
     * ندفعها بالعكس (arg11 أولاً) فيكون arg6 في [rsp+0x00] عند النداء.
     * الإزاحة تبقى 0xA0 لأن كل دفع يزيح rsp بمقدار 8.
     */
    mov  0xA0(%rsp), %rax /* arg11 */
    push %rax
    mov  0xA0(%rsp), %rax /* arg10 */
    push %rax
    mov  0xA0(%rsp), %rax /* arg9 */
    push %rax
    mov  0xA0(%rsp), %rax /* arg8 */
    push %rax
    mov  0xA0(%rsp), %rax /* arg7 */
    push %rax
    mov  0xA0(%rsp), %rax /* arg6 */
    push %rax

    /*
     * PLACEHOLDER — يُستبدل وقت التشغيل بعنوان ntb0_dispatch:
     *   48 B8 <imm64>      movabs $<dispatcher>, %rax
     *   FF D0              call *%rax
     * السبب: نسخ الـ trampoline إلى صفحة أخرى يجعل أي مرجع RIP-relative
     * خاطئاً، فنستخدم عنواناً مطلقاً (imm64) — بلا حدّ مسافة.
     */
    movabs $0, %rax
    call *%rax

    mov  %rax, %r11       /* النتيجة (NTSTATUS) */
    add  $0x30, %rsp      /* arg6..arg11 (6 × 8) */
    add  $8, %rsp         /* حشو المحاذاة */
    pop  %rsi
    pop  %rdi
    pop  %r15
    pop  %r14
    pop  %r13
    pop  %r12
    pop  %rbp
    pop  %rbx
    mov  %r11, %rax       /* النتيجة في rax كما يتوقع ويندوز */
    ret
    .size ntb_trampoline_template,.-ntb_trampoline_template

    .globl ntb_trampoline_template_end
ntb_trampoline_template_end:

    .section .note.GNU-stack,"",@progbits

/*
 * ntb0_call_win64 — مُساعد استدعاء بالاصطلاح الحقيقي لويندوز x64.
 *
 * لازم لأن الـ inline asm في C لا يضمن عدم تعارض السجلات
 * (-distribution عشوائية للسجلات تفسد النداء).
 *
 * المدخلات (اصطلاح SysV):
 *     rdi = عنوان الدالة
 *     rsi = arg1   rdx = arg2   rcx = arg3   r8 = arg4   r9 = arg5
 *     arg6..arg11 على المكدس: [rsp+0x00] عند النداء = arg6
 *
 * التحويل إلى اصطلاح ويندوز:
 *     rcx = arg1   rdx = arg2   r8 = arg3   r9 = arg4
 *     [rsp+0x20] = arg5  [rsp+0x28] = arg6  ...  [rsp+0x50] = arg11
 *
 * ينشئ 32 بايت "shadow space" كما يطلب اصطلاح ويندوز،
 * ويبقي المحاذاة 16-بايت عند نقطة النداء.
 *
 * النتيجة: rax (كما في ويندوز).
 */
    .text
    .globl ntb0_call_win64
    .type ntb0_call_win64,@function
ntb0_call_win64:
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    /*
     * الإطار المطلوب للمعاملات: 32 shadow + 7×8 معاملات = 0x58 (88 بايت)
     * → نحجز 0x60 (96 بايت) ليحتوي الإطار كاملاً، و 0x60 ≡ 0 (mod 16)
     * مع 5 pushes (40) يصبح المجموع 136 ≡ 8 (mod 16) → المحاذاة قبل
     * النداء = 0 (mod 16) كما يطلب اصطلاح SysV/Win64.
     */
    sub  $0x60, %rsp

    mov  %rdi, %r10           /* عنوان الدالة */
    mov  %rsi, %rbx           /* arg1 */
    mov  %rdx, %r12           /* arg2 */
    mov  %rcx, %r13           /* arg3 */
    mov  %r8,  %r14           /* arg4 */
    mov  %r9,  %r15           /* arg5 */

    mov  %rbx, %rcx           /* arg1 -> rcx */
    mov  %r12, %rdx           /* arg2 -> rdx */
    mov  %r13, %r8            /* arg3 -> r8  */
    mov  %r14, %r9            /* arg4 -> r9  */
    mov  %r15, 0x20(%rsp)     /* arg5 -> [rsp+0x20] قبل النداء */

    /*
     * arg6..arg11: في SysV هي معاملات مكدس للمُساعد نفسه:
     *   arg6 عند [rsp_entry+0x08] → بعد الدفعات: 0x08 + 0x88 = 0x90
     *   arg7 = 0x98  arg8 = 0xA0  arg9 = 0xA8  arg10 = 0xB0  arg11 = 0xB8
     * الوجهة (اصطلاح ويندوز): 0x28 .. 0x50
     */
    mov  0x90(%rsp), %rax
    mov  %rax, 0x28(%rsp)
    mov  0x98(%rsp), %rax
    mov  %rax, 0x30(%rsp)
    mov  0xA0(%rsp), %rax
    mov  %rax, 0x38(%rsp)
    mov  0xA8(%rsp), %rax
    mov  %rax, 0x40(%rsp)
    mov  0xB0(%rsp), %rax
    mov  %rax, 0x48(%rsp)
    mov  0xB8(%rsp), %rax
    mov  %rax, 0x50(%rsp)

    call *%r10

    add  $0x60, %rsp
    pop  %r15
    pop  %r14
    pop  %r13
    pop  %r12
    pop  %rbx
    ret
    .size ntb0_call_win64,.-ntb0_call_win64

/*
 * ntb0_thread_entry — جسر استدعاء الخيط (T-040).
 *
 * المشكلة: نقطة دخول خيط NT لها اصطلاح **ويندوز x64**:
 *     rcx = المعامل الأول، والبقية على rdx/r8/r9، مع 32 بايت "shadow space".
 * لكن pthread في لينكس يستخدم اصطلاح **SysV**:
 *     rdi = المعامل الأول.
 *
 * الحل: دالة صغيرة تنقل rdi ⇒ rcx (المعامل) ثم تنادي عنوان نقطة الدخول
 * كما هو، مع حجز 32 بايت shadow space قبل النداء (كما يطلب اصطلاح ويندوز)
 * مع إبقاء المحاذاة 16-بايت عند نقطة النداء.
 *
 *     ntb0_thread_entry(param in rdi, start_addr in rsi) -> eax
 *
 * ملاحظة: ترتيب المعاملين (param, start) **مقصود**: المعامل في rdi
 * (ليذهب إلى rcx) وعنوان نقطة الدخول في rsi (ليُنادى بـ call *%rsi).
 */
    .text
    .globl ntb0_thread_entry
    .type ntb0_thread_entry,@function
ntb0_thread_entry:
    /*
     * عند الدخول: rsp = 8 (mod 16).
     * نحجز 0x28 = 40 بايت = 32 shadow + 8 محاذاة ⇒ rsp = 0 (mod 16)
     * ⇒ النداء محاذى كما يطلب اصطلاح ويندوز.
     */
    sub  $0x28, %rsp
    mov  %rdi, %rcx        /* المعامل ⇒ rcx (اصطلاح ويندوز) */
    call *%rsi            /* نقطة الدخول (Windows ABI) */
    add  $0x28, %rsp       /* rax = قيمة الإرجاع محفوظة */
    ret
    .size ntb0_thread_entry,.-ntb0_thread_entry
