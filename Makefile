obj-m+=proxyfd.o
proxyfd-objs := main.o pipe.o

user:CFLAGS+=-g

all: user
	make -C /lib/modules/$(shell uname -r)/build/ M=$(PWD) modules
clean:
	make -C /lib/modules/$(shell uname -r)/build/ M=$(PWD) clean
