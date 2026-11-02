#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <asm/semaphore.h>

#include "swd_gpio.h"
#include "swd_h779_ioctl.h"
#include "h779_flash.h"

#define SWD_MAJOR          246
#define SWD_DEVICE_NAME    "swd_gd32"
#define SWD_DRIVER_VERSION "R2.0.0"

/* 一次 ioctl 独占整条 GPIO 总线；会话归属由 H779 层维护。 */
static DECLARE_MUTEX(swd_bus_sem);

static int swd_ioctl(struct inode *inode, struct file *file,
                     unsigned int cmd, unsigned long arg)
{
    int ret;

    (void)inode;
    if (_IOC_TYPE(cmd) != SWD_H779_IOC_MAGIC) {
        printk(KERN_ERR "h779: unsupported ioctl command=0x%x\n", cmd);
        return -ENOTTY;
    }
    if (down_interruptible(&swd_bus_sem))
        return -ERESTARTSYS;
    ret = h779_ioctl(file, cmd, arg);
    up(&swd_bus_sem);
    return ret;
}

static int swd_release(struct inode *inode, struct file *file)
{
    (void)inode;
    /* close 必须完成会话回收，不能因信号中断而遗留 owner。 */
    down(&swd_bus_sem);
    h779_release(file);
    up(&swd_bus_sem);
    return 0;
}

/* Linux 2.4 用 owner 管理模块引用；不再重复增减 MOD_USE_COUNT。 */
static struct file_operations swd_fops = {
    .owner   = THIS_MODULE,
    .release = swd_release,
    .ioctl   = swd_ioctl,
};

static int __init swd_drv_init(void)
{
    int ret;

    /* 先占用设备号，避免旧驱动仍在使用时改动它的 GPIO。 */
    down(&swd_bus_sem);
    ret = register_chrdev(SWD_MAJOR, SWD_DEVICE_NAME, &swd_fops);
    if (ret < 0) {
        printk(KERN_ERR "h779: register_chrdev major=%d failed=%d\n",
               SWD_MAJOR, ret);
        up(&swd_bus_sem);
        return ret;
    }
    ret = swd_gpio_init();
    if (ret) {
        unregister_chrdev(SWD_MAJOR, SWD_DEVICE_NAME);
        up(&swd_bus_sem);
        return ret;
    }
    up(&swd_bus_sem);
    printk(KERN_INFO "h779: SWD driver %s, major=%d, Linux 2.4 PPC\n",
           SWD_DRIVER_VERSION, SWD_MAJOR);
    return 0;
}

static void __exit swd_drv_cleanup(void)
{
    /* 有打开的文件时 owner 会阻止卸载；卸载不复位或启动目标。 */
    swd_gpio_exit();
    if (unregister_chrdev(SWD_MAJOR, SWD_DEVICE_NAME))
        printk(KERN_ERR "h779: unregister_chrdev failed\n");
}

module_init(swd_drv_init);
module_exit(swd_drv_cleanup);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("wangfm; H779 refactor");
MODULE_DESCRIPTION("MPC5200 GPIO SWD programmer for GD32H779 (Linux 2.4)");
