#ifndef IRP_CORE_H
#define IRP_CORE_H

#include "ntbox_types.h"

int ntbox_dispatch_irp(PDRIVER_OBJECT drv, int major);
int ntbox_test_create_close(PDRIVER_OBJECT drv);
NTSTATUS ntbox_dummy_dispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp);

#endif
