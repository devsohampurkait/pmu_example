# Simple out-of-tree module build
# Usage:
#   make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
#   sudo insmod toy_pmu.ko
#   sudo rmmod toy_pmu

obj-m += toy_pmu.o

