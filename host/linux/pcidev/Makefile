ifneq ($(KERNELRELEASE), )
# linux kernel 2.6 kbuild invocation
include $(src)/Make.kbuild
else
# command prompt invocation or linux kernel 2.4 build

KERNELDIR = 
CC = cl
LSMOD = 
INSMOD = 
RMMOD = 
DEPMOD = 
KERNEL_MODULE_SUFFIX = 

MODULE_NAME = pcidev
MODULE_FILE = $(MODULE_NAME).$(KERNEL_MODULE_SUFFIX)

PCIDEV_MODULE_MAKE_ALL = 

CFLAGS = /nologo /MT /W3 /DNDEBUG /DWIN32 /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS /O2 /Gr /EHsc -Wstrict-prototypes -Wno-trigraphs -g -fno-strict-aliasing -fno-common -D__KERNEL__ -DMODULE -I$(KERNELDIR)/include -O -Wall


.PHONY : all
all : $(PCIDEV_MODULE_MAKE_ALL)


.PHONY : all-kernel24
all-kernel24 : $(MODULE_FILE)

$(MODULE_FILE) : pcidev.c kernel_pcidev.h


.PHONY : all-kernel26
all-kernel26 :
	$(MAKE) -C $(KERNELDIR) SUBDIRS=$(shell pwd) modules


.PHONY : clean
clean :
	-del *.o *~ core *.mod.* .*.cmd *.ko

.PHONY : dist-clean
dist-clean: clean
	-del Makefile


.PHONY : devices
devices:
	@DEVICE="/dev/pcidev" ; \
	if test \! -c $$DEVICE ; then \
		echo "Adding $$DEVICE ..." ; \
		mknod $$DEVICE c 240 0 ; \
		chmod a+wr $$DEVICE ; \
		ls -l $$DEVICE ; \
	else \
		echo "Device $$DEVICE already exists." ; \
	fi


# works only under linux of course
.PHONY : install
install:
	@if test $$USER != "root" ; then \
		echo "Only root can install." ; \
		exit 1 ; \
	fi ; \
	if $(LSMOD) | grep -q $(MODULE_NAME) ; then \
		echo "Unloading $(MODULE_NAME) ..." ; \
		$(RMMOD) $(MODULE_NAME) ; \
	fi ; \
	echo "Loading $(MODULE_NAME) ..." ; \
	$(INSMOD) $(MODULE_FILE) ; \
	echo "Done."

endif
