obj-m += filterchip.o
filterchip-y := fchip_codec.o fchip_posfix.o fchip_vga.o fchip_hda_bus.o fchip_int.o fchip_pcm.o fchip.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean