export PTPSUPPORT CONFIG_PTPSUPPORT_OBJ DWC_ETH_QOS_CONFIG_PTP

EXTRA_CFLAGS+=-DCONFIG_MRVL_PHY
#default values
PTPSUPPORT=y	#ptp is enabled

ifeq ($(RELEASE_PACKAGE),1)
EXTRA_CFLAGS+=-DRELEASE_PACKAGE
endif
LBITS ?= $(shell getconf LONG_BIT)
ifeq ($(LBITS),64)
CCFLAGS += -m64
EXTRA_CFLAGS+=-DSYSTEM_IS_64
else
CCFLAGS += -m32
endif

ifeq "$(PTPSUPPORT)" "y"
CONFIG_PTPSUPPORT_OBJ=y
DWC_ETH_QOS_CONFIG_PTP=-DPTPSUPPORT
EXTRA_CFLAGS+=-DCONFIG_PTPSUPPORT_OBJ
else
CONFIG_PTPSUPPORT_OBJ=y
endif


KERNEL_SRC ?= ${OUT}/obj/KERNEL_OBJ

MODULE_NAME     := kmemleak_probe
MODULE_LICENSE      := GPL
MODULE_VERSION      := 1.0
MODULE_AUTHOR       := Mukunkong <mukunkong@lixiang.com>
MODULE_DESCRIPTION  := Kprobe template for easy register kernel probes

obj-m           := $(MODULE_NAME).o
$(MODULE_NAME)-m    := init.o
$(MODULE_NAME)-m    += kprobe.o trace.o
ARCH := arm64
CROSS_COMPILE := aarch64-linux-android-
ldflags-y       += -r -T $(KBUILD_EXTMOD)/kprobe.lds
ccflags-y       += -I$(KBUILD_EXTMOD)/include

name-fix   = $(squote)$(quote)$(subst $(comma),_,$(subst -,_,$1))$(quote)$(squote)
ccflags-y += -DCONFIG_MODULE_NAME=$(call name-fix,$(MODULE_NAME))
ccflags-y += -DCONFIG_MODULE_AUTHOR=$(call name-fix,$(MODULE_AUTHOR))
ccflags-y += -DCONFIG_MODULE_VERSION=$(call name-fix,$(MODULE_VERSION))
ccflags-y += -DCONFIG_MODULE_DESCRIPTION=$(call name-fix,$(MODULE_DESCRIPTION))
ccflags-y += -DCONFIG_MODULE_LICENSE=$(call name-fix,$(MODULE_LICENSE))

all:
	$(MAKE) -C $(KERNEL_SRC) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(shell pwd) modules $(KBUILD_OPTIONS)

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(shell pwd) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) clean
