obj-m += filterchip.o
filterchip-y := fchip_codec.o fchip_posfix.o fchip_vga.o fchip_hda_bus.o fchip_int.o fchip_filter.o fchip.o

KBUILD_CFLAGS += -msse -msse2 -msse4.1 -msse4.2

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean