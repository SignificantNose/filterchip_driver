obj-m += filterchip.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

bind_intel:
	echo 0000:00:1f.3 | sudo tee /sys/bus/pci/drivers/snd_hda_intel/bind
bind_fchip:
	echo 0000:00:1f.3 | sudo tee /sys/bus/pci/drivers/filterchip/bind
unbind_intel:
	echo 0000:00:1f.3 | sudo tee /sys/bus/pci/drivers/snd_hda_intel/unbind
unbind_fchip:
	echo 0000:00:1f.3 | sudo tee /sys/bus/pci/drivers/filterchip/unbind