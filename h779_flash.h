#ifndef H779_FLASH_H
#define H779_FLASH_H

#include <linux/fs.h>

/* 调用这两个入口时，驱动必须持有同一把总线锁。 */
int h779_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
void h779_release(struct file *file);

#endif
