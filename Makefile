obj-m += pixelpanel.o
pixelpanel-objs := pixelpanel_core.o pixelpanel_hub75.o

KDIR := /lib/modules/$(shell uname -r)/build

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean