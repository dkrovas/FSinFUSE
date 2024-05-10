BINS = wfs mkfs
CC = gcc
CFLAGS = -Wall -Werror -pedantic -std=gnu18 -g
NEWFLAGS = -Wall -g
FUSE_CFLAGS = `pkg-config fuse --cflags --libs`
.PHONY: all
all: $(BINS)
# 2:
# 	$(CC) $(NEWFLAGS) test2.c -o 2
# 	gdb ./2
# 14:
# 	$(CC) $(NEWFLAGS) test14.c -o 14
# 	gdb ./14
# 19:
# 	$(CC) $(NEWFLAGS) test19.c -o 19
# 	gdb ./19
wfs:
	$(CC) $(CFLAGS) wfs.c $(FUSE_CFLAGS) -o wfs
mkfs:
	$(CC) $(CFLAGS) -o mkfs mkfs.c
.PHONY: clean
clean:
	rm -rf $(BINS)
	fusermount -uz mnt
run:
	make
	./create_disk.sh     
	./mkfs -d disk.img -i 32 -b 200  
	./wfs disk.img -f -s mnt             
debug:
		make
	./create_disk.sh     
	./mkfs -d disk.img -i 32 -b 200
	gdb --args ./wfs disk.img -f -s mnt
	
