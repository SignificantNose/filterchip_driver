obj-m += filterchip_drv.o
filterchip_drv-y:= filterchip_pcm.o filterchip.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean