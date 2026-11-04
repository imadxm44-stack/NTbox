#ifndef NTAPI_STUBS_H
#define NTAPI_STUBS_H

#include "ntbox_types.h"

void NTB_MSABI ntbox_IoCompleteRequest(PIRP Irp, int prio);
NTSTATUS NTB_MSABI ntbox_RtlInitUnicodeString(PUNICODE_STRING dst, const u16 *src);

#endif
