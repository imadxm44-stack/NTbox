#include "ntbox.h"
#include "ntbox_types.h"
#include "linux_bridge.h"

int ntbox_box_init(void);
void ntbox_box_exit(void);

/* تهيئة صندوق NT-Box (توسع بعد نجاح Skeleton) */
int ntbox_box_init(void)
{
    NTBOX_LOG("NT-Box Core: initialized\n");
    return 0;
}

void ntbox_box_exit(void)
{
    NTBOX_LOG("NT-Box Core: exited\n");
}
