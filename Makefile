obj-m+=proxyfd.o
proxyfd-objs := main.o pipe.o

user:CFLAGS+=-g
prun:CFLAGS+=-g

all: user prun guinea
	make -C /lib/modules/$(shell uname -r)/build/ M=$(PWD) modules
clean:
	make -C /lib/modules/$(shell uname -r)/build/ M=$(PWD) clean
