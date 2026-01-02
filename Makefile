CC=clang++

CFLAGS=-std=c++23 -Weverything -Wno-padded -Wno-unsafe-buffer-usage -Wno-weak-vtables -Wno-c++98-compat -Wno-c++98-compat-pedantic -Wno-missing-noreturn -Wno-covered-switch-default -Werror

LFLAGS=-lstdc++exp $(shell sdl2-config --cflags --libs) -lSDL2_image -lSDL2_mixer

.PHONY: all clean

all: release

release: cozychristmas.cpp
	$(CC) cozychristmas.cpp -o cozychristmas $(CFLAGS) $(LFLAGS) -O2

debug: cozychristmas.cpp
	$(CC) cozychristmas.cpp -o debug $(CFLAGS) $(LFLAGS) -g -fsanitize=address,undefined,leak
clean:
	rm -f cozychristmas debug
