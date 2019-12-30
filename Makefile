user:CFLAGS+=-g
prun:CFLAGS+=-g

all: user prun guinea
	cd src && make
