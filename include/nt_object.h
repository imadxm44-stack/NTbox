#ifndef NT_OBJECT_H
#define NT_OBJECT_H

#include "ntbox_types.h"

PDRIVER_OBJECT ntbox_create_driver_object(void);
void ntbox_destroy_driver_object(PDRIVER_OBJECT drv);

#endif
