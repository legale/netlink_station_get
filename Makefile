#name of output file
NAME = station_get
#build dir
BD = ./build

#Linker flags
LDLIBS +=
LDDIRS += -L$(BD)

#Compiler flags
CFLAGS += -Wall -O2
CFLAGS += -I./
CFLAGS += -I/usr/include/libnl3
LDFLAGS += -lnl-3 -lnl-genl-3

#Compiler
CC = gcc
AR = ar

#SRC=$(wildcard *.c)
LIBNAME =
SRC_LIB = main.c
SRC_BIN = main.c
SRC = $(SRC_BIN)

all: $(NAME)

$(NAME): $(SRC)
		$(CC) $(CFLAGS)  $^ -o build/$(NAME) $(LDFLAGS)

clean:
		rm -rf $(BD)/*
