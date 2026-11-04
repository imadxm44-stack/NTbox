#include "ntbox.h"
#include "ntbox_types.h"
#include "hal_mini.h"
#include "linux_bridge.h"

/* Mini-HAL: فارغ في الديمو. تمرير MMIO لاحقا عبر Bridge فقط. */
int ntbox_hal_init(void)
{
    NTBOX_LOG("hal_mini: stub init (no HW access in Demo)\n");
    return 0;
}
