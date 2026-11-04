#include "ntbox.h"
#include "ntapi_stubs.h"
#include "ntbox_types.h"
#include "linux_bridge.h"

void NTB_MSABI ntbox_IoCompleteRequest(PIRP Irp, int prio)
{
    (void)prio;
    if (!Irp)
        return;
    NTBOX_LOG("ntapi: IoCompleteRequest called -> 0x%x (STATUS_SUCCESS)\n", Irp->IoStatus.Status);
}

NTSTATUS NTB_MSABI ntbox_RtlInitUnicodeString(PUNICODE_STRING dst, const u16 *src)
{
    size_t n = 0;
    if (!dst)
        return STATUS_UNSUCCESSFUL;
    if (src) {
        while (src[n])
            n++;
    }
    dst->Length = (u16)(n * sizeof(u16));
    dst->MaximumLength = dst->Length + sizeof(u16);
    dst->Buffer = (u16 *)src;
    return STATUS_SUCCESS;
}
