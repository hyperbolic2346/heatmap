CC?=gcc
CXX?=g++
AR?=ar

CFLAGS=-fPIC -Wall -std=c99 -Wextra -I. -O0 -g -Wa,-ahl=$(@:.o=.s) -Iheatmap -pedantic -I/usr/include/mysql -DBIG_JOINS=1 -fno-strict-aliasing -DNDEBUG -fopenmp -I/usr/include/ImageMagick
CXXFLAGS=-O3 -lm -std=c++0x

LDFLAGS=-L/usr/lib/x86_64-linux-gnu -lmysqlclient -lpthread -lz -lm -ldl -lMagickWand -lMagickCore

.PHONY: all clean

all: heatmap_gen

clean:
	rm -f heatmap_gen
	find . -name '*.[os]' -print0 | xargs -0 rm -f

heatmap_gen.o: heatmap_gen.c
	$(CC) -c $< $(CFLAGS) -o $@

heatmap_gen: heatmap_gen.o heatmap/libheatmap.a 
	$(CC) -o $@ $^ $(LDFLAGS)
