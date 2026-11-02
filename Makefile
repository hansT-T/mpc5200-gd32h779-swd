# Linux 2.4 多对象模块；须使用已配置好的目标 BSP。
ifneq ($(strip $(TOPDIR)),)

O_TARGET := swd_h779_builtin.o
obj-m := swd_h779.o
list-multi := swd_h779.o
swd_h779-objs := swd.o swd_gpio.o swd_transport.o h779_flash.o

include $(TOPDIR)/Rules.make

# 显式链接兼容较早的 Linux 2.4 Rules.make。
swd_h779.o: $(swd_h779-objs)
	$(LD) $(LDFLAGS) -r -o $@ $(swd_h779-objs)

else

ARCH ?= ppc
CROSS_COMPILE ?= ppc_82xx-
KERNEL_DIR ?=

.PHONY: all modules clean
all: modules

modules:
	@test -n "$(KERNEL_DIR)" || { echo "请设置 KERNEL_DIR，指向已配置的 Linux 2.4 BSP"; exit 1; }
	@test -f "$(KERNEL_DIR)/Rules.make" || { echo "KERNEL_DIR 中没有 Linux 2.4 Rules.make"; exit 1; }
	$(MAKE) -C "$(KERNEL_DIR)" ARCH="$(ARCH)" CROSS_COMPILE="$(CROSS_COMPILE)" SUBDIRS="$(CURDIR)" modules

clean:
	$(RM) swd.o swd_gpio.o swd_transport.o h779_flash.o swd_h779.o swd_h779_builtin.o
	$(RM) .swd.o.flags .swd_gpio.o.flags .swd_transport.o.flags .h779_flash.o.flags
	$(RM) .swd_h779.o.flags .swd_h779_builtin.o.flags .depend

endif
