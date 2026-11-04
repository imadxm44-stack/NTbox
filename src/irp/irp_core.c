#include "ntbox.h"
#include "irp_core.h"
#include "ntapi_stubs.h"
#include "ntbox_types.h"
#include "linux_bridge.h"

int ntbox_dispatch_irp(PDRIVER_OBJECT drv, int major)
{
    IRP irp;
    u8 stackloc[0x40];
    u8 obj[0x80];
    if (!drv || major < 0 || major > IRP_MJ_MAX)
        return -EINVAL;
    if (!drv->MajorFunction[major]) {
        NTBOX_ERR("irp_core: no handler for major=%d\n", major);
        return -ENOSYS;
    }
    memset(&irp, 0, sizeof(irp));
    memset(stackloc, 0, sizeof(stackloc));
    memset(obj, 0, sizeof(obj));
    stackloc[0] = (u8)major;
    *(void **)(stackloc + 0x30) = obj;
    irp.CurrentStackLocation = stackloc;
    NTBOX_LOG("irp_core: dispatching IRP to MajorFunction[%d]\n", major);
    return drv->MajorFunction[major](NULL, &irp);
}

int ntbox_test_create_close(PDRIVER_OBJECT drv)
{
    int r;
    NTBOX_LOG("irp_core: preparing IRP_MJ_CREATE\n");
    r = ntbox_dispatch_irp(drv, IRP_MJ_CREATE);
    if (r != (int)STATUS_SUCCESS)
        return r;
    NTBOX_LOG("irp_core: preparing IRP_MJ_CLOSE\n");
    r = ntbox_dispatch_irp(drv, IRP_MJ_CLOSE);
    return r;
}

NTSTATUS ntbox_dummy_dispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    (void)DeviceObject;
    NTBOX_LOG("DummyDispatch called\n");
    if (Irp) {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        ntbox_IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }
    return STATUS_SUCCESS;
}
