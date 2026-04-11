obj-m += pixelpanel.o
pixelpanel-objs := pixelpanel_core.o pixelpanel_hub75.o pixelpanel_hub75_luts.o

KDIR := /lib/modules/$(shell uname -r)/build

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean