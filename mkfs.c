#include <sys/stat.h>
#include "wfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>



void print_all(char* disk) {
    struct wfs_sb* super = (struct wfs_sb*) disk;
    printf("Printing the disk---\n");
    printf("Superblock\n");
    printf("num_inodes: %ld\n", super->num_inodes);
    printf("num_data_blocks: %ld\n", super->num_data_blocks);
    printf("ibitmap: %ld\n", super->i_bitmap_ptr);
    printf("dbitmap: %ld\n", super->d_bitmap_ptr);
    printf("inodes: %ld\n", super->i_blocks_ptr);
    printf("size of inode: %ld\n", sizeof(struct wfs_inode));
    printf("blocks: %ld\n", super->d_blocks_ptr);
}
size_t roundup32(size_t n) {
    //printf("n inside function = %ld\n", n);
    return((n+31) & ~31);
}

void process_args(int argc, char *argv[], char **disk_img, size_t *num_inodes, size_t *num_blocks) {
    if (argc != 7) {
        printf("Usage: %s -d disk_img -i num_inodes -b num_blocks\n", argv[0]);
        exit(1);
    }

    for (int i = 1; i < argc; i += 2) {
        if (strcmp(argv[i], "-d") == 0) {
            *disk_img = argv[i + 1];
        } else if (strcmp(argv[i], "-i") == 0) {
            // printf("argv[i + 1]: %s\n", argv[i + 1]);
            *num_inodes = roundup32(atoi(argv[i + 1]));
            // printf("num_inodes: %ld\n", *num_inodes);
        } else if (strcmp(argv[i], "-b") == 0) {
            // printf("argv[i + 1]: %s\n", argv[i + 1]);
            *num_blocks = roundup32(atoi(argv[i + 1]));
            // printf("num_blocks: %ld\n", *num_blocks);
        } else {
            printf("Unknown argument: %s\n", argv[i]);
            exit(1);
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc != 7) {
        printf("Usage: %s -d disk_img -i num_inodes -b num_blocks\n", argv[0]);
        return 1;
    }
    char *disk_img;
    size_t num_inodes;
    size_t num_blocks;

    process_args(argc, argv, &disk_img, &num_inodes, &num_blocks);

    int fd = open(disk_img, O_RDWR);
    if (fd == -1) {
        perror("open");
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) == -1) {
        perror("fstat");
        return 1;
    }

    void *img = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (img == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    int size_ibitmap = num_inodes / 8;
    if (num_inodes % 8 != 0) {
        ++size_ibitmap;
    }
    if (size_ibitmap % 4 != 0) {
        size_ibitmap = size_ibitmap + 4 - (size_ibitmap % 4);
    }
    int size_dbitmap = num_blocks / 8;
    if (num_blocks % 8 != 0) {
        ++size_dbitmap;
    }
    if (size_dbitmap % 4 != 0) {
        size_dbitmap = size_dbitmap + 4 - (size_dbitmap % 4);
    }
    // At the moment, we are 4 byte alligning the bitmaps
    if (st.st_size < (num_blocks * BLOCK_SIZE + 512 * num_inodes + size_dbitmap + size_ibitmap + sizeof(struct wfs_sb))) {
        printf("Disk image is too small\n");
        return 1;
    }

    struct wfs_sb *sb = (struct wfs_sb *) img;
    sb->num_inodes = num_inodes;
    sb->num_data_blocks = num_blocks;
    sb->i_bitmap_ptr = sizeof(struct wfs_sb);
    sb->d_bitmap_ptr = sb->i_bitmap_ptr + (num_inodes / 8);
    sb->i_blocks_ptr = sb->d_bitmap_ptr + (num_blocks / 8);
    sb->d_blocks_ptr = sb->i_blocks_ptr + (num_inodes * BLOCK_SIZE);

    struct wfs_inode *root = (struct wfs_inode *) ((char*) img + sb->i_blocks_ptr);
    int* mmap_ibitmap = (int*)((char *)img + sb->i_bitmap_ptr);
    //parsear o bitmap por ints. 
    *mmap_ibitmap = 1;
    for(int j = 0; j < 31; ++j) {
        // printf("mmap_ibitmap is %x\n", *mmap_ibitmap);
        *mmap_ibitmap = *mmap_ibitmap * 2;
    }
    // printf("mmap_ibitmap is %x\n", *mmap_ibitmap);
    root->num = 0; // root inode number is usually 0
    root->mode = S_IFDIR | 0755; // root is a directory with 755 permissions
    root->uid = getuid(); // owner is the current user
    root->gid = getgid(); // group is the current user's group
    root->size = 0; // size is 0 for an empty directory
    root->nlinks = 2; // . and .. links
    root->atim = time(NULL); // current time
    root->mtim = time(NULL); // current time
    root->ctim = time(NULL); // current time
    for (int i = 0; i < 7; i++) {
        root->blocks[i] = 0; // no data blocks yet
    }

    if (msync(img, st.st_size, MS_SYNC) == -1) {
        perror("msync");
        return 1;
    }
    //print_all(img);
    if (munmap(img, st.st_size) == -1) {
        perror("munmap");
        return 1;
    }
    close(fd);
    // printf("no segfault\n");
    // char str[] = ".eba.que.legal.";
    // printf("%s\n", strtok(str, "."));
    // printf("%s\n", strtok(NULL, "."));
    // printf("%s\n", strtok(NULL, "."));
    // printf("%lx\n", (unsigned long) strtok(NULL, "."));

    return 0;
}
