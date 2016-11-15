all: i2l

CFLAGS = -Wall -Wextra -g
LDFLAGS = -g

i2l.o: i2l.h

i2l: i2l.o
