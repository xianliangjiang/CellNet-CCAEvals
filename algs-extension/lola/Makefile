obj-m := tcp_lola.o

KDIR    := /lib/modules/$(shell uname -r)/build
KDIR1	:= /home/felix/Daten/Uni/Projekte/Bachelorarbeit/Kernel/linux-4.6.1/

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
