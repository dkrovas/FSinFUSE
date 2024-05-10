
#include "wfs.h"
#include <fuse.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
int wfs_getattr(const char *path, struct stat *stbuf);
int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi);
int wfs_mkdir(const char *path, mode_t mode);
int wfs_rmdir(const char *path);
int wfs_mknod(const char *path, mode_t mode, dev_t dev);
int wfs_unlink(const char *path);
int wfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
char* image;
struct wfs_sb* super;
struct fuse_operations ops = {
    .getattr = wfs_getattr,
    .mknod   = wfs_mknod,
    .mkdir   = wfs_mkdir,
    .unlink  = wfs_unlink,
    .rmdir   = wfs_rmdir,
    .read    = wfs_read,
    .write   = wfs_write,
    .readdir = wfs_readdir,
};
size_t bitmap_count(const char* start, size_t size) {
  size_t count = 0;
  for (size_t i = 0; i < size; i++) {
    for (int j = 0; j < 8; j++) {
      if ((start[i] & (1 << j)) != 0) {
        count++;
      }
    }
  }
  return count;
}

size_t inode_count(char* disk_map) {
  struct wfs_sb* sb = (struct wfs_sb*)disk_map;
  char* i_bitmap = disk_map + sb->i_bitmap_ptr;
  return bitmap_count(i_bitmap, sb->num_inodes / 8);
}

size_t data_block_count(char* disk_map) {
  struct wfs_sb* sb = (struct wfs_sb*)disk_map;
  char* d_bitmap = disk_map + sb->d_bitmap_ptr;
  return bitmap_count(d_bitmap, sb->num_data_blocks / 8);
}
int get_bitmap(int* ptr, int position) {
    if (ptr == NULL) {
        printf("Error: ptr is NULL\n");
        return -1;
    }
    if (position < 0) {
        printf("Error: position is negative\n");
        return -1;
    }
    // printf("position:%d\n", position);
    ptr = ptr + (position / 32);
    int mask = 1 << (31 - (position % 32));
    return (*ptr & mask);
}
int find_first_available_bitmap(int type) { // type 0 for data, 1 for inode
    struct wfs_sb *super = (struct wfs_sb *) image;
    if (type) {
        for(int i = 0; i < super->num_inodes; ++i) {
            if(!get_bitmap((int*)(image + super->i_bitmap_ptr), i)) {
                return i;
            }
        }
    } else {
        for(int i = 0; i < super->num_data_blocks; ++i) {
            if(!get_bitmap((int *)(image + super->d_bitmap_ptr), i)) {
                return i;
            }
        }
    }
    return -1;
}

int set_bitmap(int* ptr, int position, int value) {
    ptr = ptr + (position / 32);
    int new = 1;
    for(int i = 0; i < (31 -(position % 32)); ++i) {
        new = new * 2;
    }
    if (value) {
        *ptr = *ptr + new;
    } else {
        *ptr = *ptr - new;
    }
    return 0;
}
void fill_stat(struct wfs_inode *inode, struct stat *stbuf){
    memset(stbuf, 0, sizeof(struct stat));
    stbuf->st_mode = inode->mode;
    stbuf->st_uid = inode->uid;
    stbuf->st_gid = inode->gid;
    stbuf->st_size = inode->size;
    stbuf->st_nlink = inode->nlinks;
    stbuf->st_atime = inode->atim;
    stbuf->st_mtime = inode->mtim;
    stbuf->st_ctime = inode->ctim;
    stbuf->st_ino = inode->num;
    int blocks = 0;
    if(!S_ISDIR(inode->mode)) {
        stbuf->st_blocks = ((inode->size + 511) & (~511)) / 512;
    } else {
        for(int i = 0; i < 7; ++i) {
            if (inode->blocks[i] != 0) {
                blocks++;
            }
        }
        stbuf->st_blocks = blocks;
    }
}
void shift_dentries(struct wfs_inode* inode, int blocks, int entry_num) {
    struct wfs_dentry* entry = ((struct wfs_dentry*)(image + inode->blocks[blocks])) + entry_num;
    struct wfs_dentry* next_entry = ((struct wfs_dentry*) (image + inode->blocks[blocks])) + entry_num + 1;
    for(int i = entry_num; i < 16; ++i) {
        if(next_entry->num == -1) {
            return;
        }
        if (i != 15) {
            entry->num = next_entry->num;
            strcpy(entry->name, next_entry->name);
        } else {
            blocks++;
            if(blocks >= 8 || inode->blocks[blocks] == 0) {
                return;
            } else {
                next_entry = ((struct wfs_dentry*)(image + inode->blocks[blocks]));
                entry->num = next_entry->num;
                strcpy(entry->name, next_entry->name);
                entry = ((struct wfs_dentry*)(image + inode->blocks[blocks])) - 1;
            }
        }
        ++entry;
        ++next_entry;
    }
}
struct wfs_inode *find_inode(const char *path){
    struct wfs_inode *curr_inode = (struct wfs_inode *)(image + super->i_blocks_ptr);
    if (!strcmp(path, "/")) {
        return curr_inode;
    }
    char *curr_name = strtok((char * restrict)path, "/");
    struct wfs_dentry *curr_dentry;
    int found = 0;
    int path_length = 0;
    int path_found = 0;
    while(curr_name != NULL) {
        for(int i = 0; i < 7; ++i) { //special case when i == 7, handle it
            if (curr_inode->blocks[i] == 0) {
                continue;
            }
            curr_dentry = (struct wfs_dentry *) ((int)(curr_inode->blocks[i]) + image);
            for(int k = 0; k < 16; ++k) {
                if (curr_dentry->num == -1) {
                    curr_dentry++;
                    continue;
                }
                if (strcmp(curr_dentry->name, curr_name) == 0) {
                    int inode = curr_dentry->num;
                    curr_inode =(struct wfs_inode*)(((char *)(image + super->i_blocks_ptr)) + inode*512);
                    found = 1;
                    path_found++;
                    break;
                } else {
                    curr_dentry++;
                }
            }
            if (found) {
                found = 0;
                break;
            }
        }
        curr_name = strtok(NULL, "/");
        path_length++;
    }
    if (path_found != path_length) {
        // printf("retornando null\n");
        return NULL;
    }
    return curr_inode;
}
int get_parent_inode(const char *path, struct wfs_inode_and_child* rtvalue, int mode) {
    //printf("path in get_parent_inode is %s\n", path);
    char *copy_path = strdup(path);
    char *copy_path2 = strdup(path);
    //char *copy_path3 = strdup(path);
    struct wfs_inode *curr_inode = (struct wfs_inode *)((char *)image + super->i_blocks_ptr);
    char *curr_name = strtok((char * restrict)copy_path, "/");
    struct wfs_dentry *curr_dentry;
    int num_dirs = mode;
    while (curr_name != NULL) {
        ++num_dirs;
        curr_name = strtok(NULL, "/\0");
    }
    int found = 0;
    curr_name = strtok(copy_path2, "/");
    int path_found = 1;
    for(int j = 0; j < num_dirs - 1; ++j) {
        for(int i = 0; i < 7; ++i) { //special case when i == 7, handle it
            if (curr_inode->blocks[i] == 0) {
                continue;
            }
            curr_dentry = (struct wfs_dentry *) ((int)(curr_inode->blocks[i]) + image);
            for(int k = 0; k < 16; ++k) {
                if (curr_dentry->num == -1) {
                    curr_dentry++;
                    continue;
                }
                if (strcmp(curr_dentry->name, curr_name) == 0) {
                    int inode = curr_dentry->num;
                    curr_inode =(struct wfs_inode*) (((char *)(image + super->i_blocks_ptr)) + inode*512);
                    found = 1;
                    path_found++;
                    break;
                }
                curr_dentry++;
            }
            if (found) {
                found = 0;
                break;
            }
        }
        curr_name = strtok(NULL, "/");
    }
    printf("curr_inode->num after loop is %d\n", curr_inode->num);
    rtvalue->inode = curr_inode; 
    curr_name = strtok(NULL, "/"); // esse loop aqui ta errado pq curr_name ta null 
    if(curr_name == NULL) { 
        curr_name = strdup(path); 
        curr_name = strtok(curr_name, "/");
        int offset;
        if (mode){
            offset = 2;
        }
        else {
            offset = 1;
        }
        for(int m = 0; m < num_dirs - offset; ++m) {
            curr_name = strtok(NULL, "/"); 
        } 
    }
    for(int o = 0; o < MAX_NAME; ++o) { 
        rtvalue->child[o] = curr_name[o]; 
    }
    printf("rtvalue->child is %s\n", rtvalue->child);
    return 0;
}
long get_new_data_block(struct wfs_inode* inode) { //returns the new index
    long i;
    for(i = 0; i < 8; ++i) {
        if (inode->blocks[i] == 0) {
            break;
        }
    }
    int block_index = find_first_available_bitmap(0);
    if (block_index == -1) {
        return -1;
    }
    set_bitmap((int *)(image + super->d_bitmap_ptr), block_index, 1);
    inode->blocks[i] = (long) (super->d_blocks_ptr) + 512 * block_index;
    if(i >= 7) {
        return (long) (super->d_blocks_ptr) + 512 * block_index; //returns the address if inode that called needs indirect pointers
    } else {
        return i; //returns position in the array of blocks if the inode that called can use direct pointers
    }
}
struct wfs_inode* get_new_inode_block() {
    int inode = find_first_available_bitmap(1);
    if(inode == -1) {
        return NULL;
    }
    set_bitmap((int*)(image + super->i_bitmap_ptr), inode, 1);
    struct wfs_inode* new_inode = (struct wfs_inode*)(((char *)(image + super->i_blocks_ptr)) + inode*512);
    new_inode->num = inode;
    return new_inode;
}
int clear_block (char* ptr, int mode) { //0 for directory, 1 for file
    if (mode) {
        long* pointer = (long*) ptr;
        for(int i = 0; i < 64; ++i) {
            *pointer = 0;
            ++pointer;
        }
    } else {
        struct wfs_dentry* entry = (struct wfs_dentry*) ptr;
        for (int i = 0; i < 16; ++i) {
            entry->num = -1;
            ++entry;
        }
    }
    return 0;
}
// pega os primeiros 4 bytes do bitmap, cria uma copia na stack, pega a sobra de dividir por 2. Se for impar, o bit 0 ta sendo usado.
// divide o numero por 2, salva ele na variavel e pega a sobra de dividir por 2. Se for impar, o bit 1 ta sendo usado
int wfs_getattr(const char *path, struct stat *stbuf) {
    printf("Calling getattributes\n");
    
    struct wfs_inode *curr_inode = find_inode(path);
    if(curr_inode == NULL) {
        return -ENOENT;
    } else {
        printf("path is %s\n", path);
        printf("curr_inode->num in get_attr: %d\n",curr_inode->num);
        printf("curr_inode is: %p\n", (void *)curr_inode);
    }
    fill_stat(curr_inode, stbuf);
    // printf("stbuf->st_mode is %d\n", stbuf->st_mode);
    // printf("stbuf->st_uid is %d\n", stbuf->st_uid);
    // printf("stbuf->st_gid is %d\n", stbuf->st_gid);
    // printf("stbuf->st_size is %ld\n", stbuf->st_size);
    // printf("stbuf->st_nlink is %ld\n", stbuf->st_nlink);
    // printf("stbuf->inode is %ld\n", stbuf->st_ino);
    // printf("stbuf->st_blocks is %ld\n", stbuf->st_blocks);
    // printf("node %s exists\n", path);
    return 0;
}

int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    printf("Calling readdir on path %s\n", path);
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    struct wfs_inode *curr_inode = find_inode(path);
    
    if(curr_inode == NULL) {
        // printf("SHOULD NOT PRINT THIS IN READDIR\n");
        return -ENOENT;
    }
    // printf("curr_inode in readdir: %p\n", (void *)curr_inode);
    if(!S_ISDIR(curr_inode->mode)) {
        return -ENOTDIR;
    }
    // printf("returned curr_inode is %d\n", curr_inode->num);
    struct wfs_dentry *curr_dentry;
    for(int i = 0; i < 7; ++i) { //special case when i == 7, handle it
        if (curr_inode->blocks[i] == 0) {
            continue;
        }
        curr_dentry = (struct wfs_dentry *) ((int)curr_inode->blocks[i] + image);
        for(int k = 0; k < 16; ++k) { 
            // printf("curr_dentry->num:%d\n", curr_dentry->num);
            if (curr_dentry->num == -1) {
                curr_dentry++;
                continue;
            }
            filler(buf, curr_dentry->name, NULL, 0);
            curr_dentry++;
        }
    }
    // loop throough all dentrys, for each one, copy the name to the buffer
    // filler(buf, name, NULL, 0);
    
    return 0;
}

int wfs_mkdir(const char *path, mode_t mode) {
    printf("Calling mkdir\n");
    char *copy_path = strdup(path);
    struct wfs_inode *inode = find_inode(copy_path); //tries to find inode
    if (inode != NULL) { //if not null, it already exists
        return -EEXIST;
    }
    struct wfs_dentry *curr_dentry;
    struct wfs_inode_and_child parent;
    if (get_parent_inode(path, &parent, 1) == -1) { //if parent not found, return enoent
        return -ENOENT;
    }
    struct wfs_inode *curr_inode = parent.inode; //inode of the parent
    char *curr_name = parent.child; //name of the child
    struct wfs_dentry* free_dentry = NULL;
    int used_blocks = 0;
    for(int i = 0; i < 7; ++i) { //Go through each of the parents data blocks
        if (curr_inode->blocks[i] == 0) { //if 0, not yet allocated, just continue
            continue;
        }
        used_blocks += 1;
        curr_dentry = (struct wfs_dentry *) ((int)curr_inode->blocks[i] + image); //found a valid block, let's check all directory entries
        for(int k = 0; k < 16; ++k) { //checks at most 16 directory entries
            // printf("k is %d\n", k);
            if (curr_dentry->num == -1) { //if num is -1, not allocated entry. It is a free entry
                if(free_dentry == NULL) { //if found a free entry, let's use it for our new directory later
                    free_dentry = curr_dentry;
                }
                curr_dentry++; //just continue looking;
                continue;
            }
            curr_dentry++;//loop through all directory entries in this block
        }
    }
    //name guaranteed to not be used in this directory
    struct wfs_inode* new_inode = get_new_inode_block(); //gets a new inode for the new directory
    if(new_inode == NULL) {//if NULL, it is out of space
        return -ENOSPC;
    }
    struct wfs_dentry* entry;
    if(free_dentry == NULL) {//No free entry, needs more blocks
        if(used_blocks > 7) { //directory has no blocks left to allocate
            return -ENOSPC;
        }
        long returned = get_new_data_block(curr_inode);
        if (returned ==  -1) {//no more free blocks in the system
            //printf("SHOULD NOT BE HERE\n");
            return -ENOSPC;
        }
        clear_block(image + curr_inode->blocks[returned], 0);//making all entries empty
        entry = (struct wfs_dentry*) (image + curr_inode->blocks[returned]);//setting the entry that will be filled with new directory
    } else {
        entry = free_dentry;// there is a free entry, so we will use it
    }
    // printf("inside mkdir, curr_inode->num is %d, path is %s\n", curr_inode->num, path);
    curr_inode->mtim = time(NULL);
    curr_inode->ctim = time(NULL); //setting new access times, because we are modifying this directory
    curr_inode->atim = time(NULL);
    curr_inode->size += sizeof(struct wfs_dentry);
    curr_inode->nlinks++; //setting new link, because we are creating a child
    strcpy((char*)entry->name, curr_name); //copying the name of the new directory to the directory entry in the parent
    entry->num = new_inode->num; //setting the inode of the directory entry
    new_inode->mode = mode | __S_IFDIR;
    new_inode->uid = getuid();
    new_inode->gid = getgid();
    new_inode->nlinks = 2; // 2 nlinks, because the directory will reference itself and the parent also has a link to it
    new_inode->mtim = time(NULL);
    new_inode->ctim = time(NULL);
    new_inode->atim = time(NULL);
    new_inode->size = 0;
    for(int j = 0; j < 7; ++j) { //clearing all blocks to 0
        new_inode->blocks[j] = 0;
    }
    //printf("curr:%d and new_inode num shows %d and its address is %lx\n", ((struct wfs_dentry *) (image + curr_inode->blocks[0]))->num, new_inode->num, (long unsigned int)new_inode);
    return 0;
}

int wfs_rmdir(const char *path) {
    printf("Calling rmdir\n");
    char *copy_path = strdup(path);
    struct wfs_inode *curr_inode = find_inode(copy_path);
    if(curr_inode == NULL) {
        return -ENOENT;
    }
    if(!S_ISDIR(curr_inode->mode)) {
        return -ENOTDIR;
    }
    struct wfs_inode_and_child parent;
    if(get_parent_inode(path, &parent, 0) == -1) {
        return -ENOENT;
    }
    //set_bitmap(curr_inode, curr_inode->num, 0);
    curr_inode = parent.inode;
    char *curr_name = parent.child;
    struct wfs_dentry *curr_dentry;
    for(int i = 0; i < 7; ++i) {
        if (curr_inode->blocks[i] == 0) {
            continue;
        }
        curr_dentry = (struct wfs_dentry *) ((int)(curr_inode->blocks[i]) + image);
        for(int k = 0; k < 16; ++k) { 
            if (curr_dentry->num == -1) {
                curr_dentry++;
                continue;
            }
            if (strcmp(curr_dentry->name, curr_name) == 0) {
                curr_inode->size -= sizeof(struct wfs_dentry);
                curr_inode->mtim = time(NULL);
                curr_inode->ctim = time(NULL);
                curr_inode->atim = time(NULL);
                curr_inode->nlinks--;
                struct wfs_inode* inode = (struct wfs_inode*)((char *)(image + super->i_blocks_ptr) + curr_dentry->num*512);
                for(int j = 0; j < 7; ++j) {
                    if(inode->blocks[j] != 0) {
                        set_bitmap((int *)(image + super->d_bitmap_ptr), (inode->blocks[j] - super->d_blocks_ptr) / 512, 0);
                    }
                }
                set_bitmap((int *)(image + super->i_bitmap_ptr), curr_dentry->num, 0);
                curr_dentry->num = -1;
                printf("inode count is %ld\n", inode_count(image));
                return 0;
            }
            curr_dentry++;
        }
    }
    printf("inode count is %ld\n", inode_count(image));
    return 0;
}

int wfs_mknod(const char *path, mode_t mode, dev_t dev) {
    printf("Calling mknod\n");
    char *copy_path = strdup(path);
    struct wfs_inode *inode = find_inode(copy_path); //tries to find inode
    if (inode != NULL) { //if not null, it already exists
        return -EEXIST;
    }
    struct wfs_dentry *curr_dentry;
    struct wfs_inode_and_child parent;
    if (get_parent_inode(path, &parent, 1) == -1) {
        return -ENOENT;
    }
    struct wfs_inode *curr_inode = parent.inode;//inode of the parent
    char *curr_name = parent.child;//name of the child
    struct wfs_dentry* free_dentry = NULL;
    int used_blocks = 0;
    for(int i = 0; i < 7; ++i) { //going through each data block to find a new entry and to see if there exists and inode with the same name
        if (curr_inode->blocks[i] == 0) {//if 0, not yet allocated
            continue;
        }
        used_blocks += 1;
        curr_dentry = (struct wfs_dentry *) ((int)curr_inode->blocks[i] + image);//block is being used, going to check all directory entries
        for(int k = 0; k < 16; ++k) {
            // printf("k is %d\n", k);
            if (curr_dentry->num == -1) {
                if(free_dentry == NULL) { //found a free entry, let's use it later
                    free_dentry = curr_dentry;
                }
                curr_dentry++;//just a free entry, continue
                continue;
            }
            curr_dentry++;//just continue
        }
    }
    struct wfs_inode* new_inode = get_new_inode_block(); //gets a new inode for the new file
    if(new_inode == NULL) {
        printf("AND HERE\n");
        return -ENOSPC; //no more free inodes
    }
    struct wfs_dentry* entry;
    if(free_dentry == NULL) {//No free entry, needs more blocks
        if(used_blocks > 7) { //directory has no blocks left to allocate
            printf("OR HERE\n");
            return -ENOSPC;
        }
        long returned = get_new_data_block(curr_inode);
        if (returned ==  -1) {//no more free blocks in the system
            printf("SHOULD NOT BE HERE\n");
            return -ENOSPC;
        }
        clear_block(image + curr_inode->blocks[returned], 0);//making all entries empty
        entry = (struct wfs_dentry*) (image + curr_inode->blocks[returned]);//setting the entry that will be filled with new directory
    } else {
        entry = free_dentry;// there is a free entry, so we will use it
    }
    curr_inode->mtim = time(NULL);
    curr_inode->ctim = time(NULL); //updating the time
    curr_inode->atim = time(NULL);
    curr_inode->size += sizeof(struct wfs_dentry);//updating the size of the directory
    strcpy((char*)entry->name, curr_name);//copying the name to the directory entry
    entry->num = new_inode->num;//copying the num to the directory entry
    new_inode->uid = getuid();
    new_inode->gid = getgid();
    new_inode->mode = mode;
    new_inode->nlinks = 1;
    new_inode->mtim = time(NULL);
    new_inode->ctim = time(NULL);
    new_inode->atim = time(NULL);
    new_inode->size = 0;//size is 0
    for(int j = 0; j < 8; ++j) {
        new_inode->blocks[j] = 0; //clearing the data blocks
    }
    return 0;
}

int wfs_unlink(const char *path) {
    printf("Calling unlink\n");
    char *copy_path = strdup(path);
    struct wfs_inode *inode = find_inode(copy_path);
    if (inode == NULL) {
        return -ENOENT;
    }
    if (S_ISDIR(inode->mode)) {
        return -EISDIR;
    }
    struct wfs_inode_and_child parent;
    if (get_parent_inode(path, &parent, 0) == -1) {
        return -ENOENT;
    }
    struct wfs_inode *curr_inode = parent.inode;
    char *curr_name = parent.child;
    printf("curr_name in unlink is %s curr_inode->num is %d\n", curr_name, curr_inode->num);
    struct wfs_dentry *curr_dentry;
    for(int i = 0; i < 7; ++i) {
        if (curr_inode->blocks[i] == 0) {
            // printf("curr_inode->blocks[%d] is 0\n", i);
            continue;
        }
        curr_dentry = (struct wfs_dentry *) ((int)(curr_inode->blocks[i]) + image);
        for(int k = 0; k < 16; ++k) {
            // printf("curr_dentry->num is %d curr_dentry->name is %s\n", curr_dentry->num, curr_dentry->name);
            // printf("curr_inode->num is %d\n", curr_inode->num);
            if (curr_dentry->num == -1) {
                curr_dentry++;
                continue;
            }
            if (strcmp(curr_dentry->name, curr_name) == 0) {
                // printf("FOUND INODE IN UNLINK\n");
                for (int j = 0; j < 8; ++j) {
                    if (inode->blocks[j] != 0) {
                        if (j == 7) {
                            long* pointer = (long*)(image + inode->blocks[7]);
                            while(*pointer != 0) {
                                set_bitmap((int*)(image + super->d_bitmap_ptr), *(pointer - super->d_blocks_ptr) / 512, 0);
                                ++pointer;
                            }
                        } else {
                            set_bitmap((int *)(image + super->d_bitmap_ptr), (inode->blocks[j] - super->d_blocks_ptr) / 512, 0);
                        }
                        // printf("inode->blocks[%d] is %lx\n", j, inode->blocks[j]);
                    }
                }
                curr_inode->size -= sizeof(struct wfs_dentry);
                // shift_dentries(curr_inode, i, k);
                // if(curr_inode->size % 512 == 0) {
                //     set_bitmap((int*)(image + super->d_bitmap_ptr), (curr_inode->blocks[curr_inode->size / 512] - super->d_blocks_ptr) / 512, 0);
                // }
                curr_inode->mtim = time(NULL);
                curr_inode->ctim = time(NULL);
                curr_inode->atim = time(NULL);
                set_bitmap((int *)(image + super->i_bitmap_ptr), curr_dentry->num, 0);
                curr_dentry->num = -1;
                return 0;
            }
            curr_dentry++;
        }
    }

    return 0;
}

int wfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    printf("Calling read\n");
    struct wfs_inode *curr_inode = find_inode(path);
    if(curr_inode == NULL) {
        return -ENOENT;
    }
    // if(!S_ISREG(curr_inode->mode)) {
    //     return -EISDIR;
    // }
    int block_number = offset / 512; //this will tell us where to start the reads
    char* pointer;
    long* address_pointer_offset = NULL;
    int indirect_index = -1;
    int bytes_read = 0;
    if(block_number > 6) { //if block number is less than 6, setting the initial pointer to the block. If greater, setting it according to indirect block
        indirect_index = block_number - 7;
        address_pointer_offset = ((long*)(image + curr_inode->blocks[7])) + indirect_index;
        pointer = image + *address_pointer_offset;
    } else {
        pointer = image + curr_inode->blocks[block_number];
    }
    int first_block_offset = offset % 512;
    int bytes_left = size;
    int bytes_that_can_be_read = curr_inode->size - offset;
    int first_read = 1;
    long quantity;
    if (curr_inode->size - offset < size) {
        bytes_left = curr_inode->size - offset;
    } else {
        bytes_left = size;
    }    while(bytes_left != 0) {
        if(block_number >= 7) { //if we reach block 7, we need to go to the next loop for the indirect blocks
            break;
        }
        if(first_read) { //first read, need to consider offset
            if(bytes_left < 512 - first_block_offset) {
                quantity = bytes_left;
            } else {
                quantity = 512 - first_block_offset;
            }
            pointer += first_block_offset;
            memcpy(buf, pointer, quantity);
            bytes_left = bytes_left - quantity;
            buf = buf + quantity;
            block_number++;
            pointer = image + curr_inode->blocks[block_number];
            bytes_read = bytes_read + quantity;
            first_read = 0;
            continue;
        }
        if (bytes_left >= 512) { //subsequent read, can copy whole 512 bytes because first read guarantees it
            memcpy(buf, pointer, 512);
            bytes_left -= 512;
            buf += 512;
            block_number++;
            pointer = image + curr_inode->blocks[block_number];
            bytes_read += 512;
        } else { //last read if we don't need to get into the indirect block
            memcpy(buf, pointer, bytes_left);
            bytes_read += bytes_left;
            bytes_left = 0;
        }
    }
    if(address_pointer_offset == NULL) {//first time using indirect block, need to set the pointer accordingly. 
    //If offset makes us write here first, pointer already set in one of the first ifs
        indirect_index = 0;
        address_pointer_offset = (long*)(image + curr_inode->blocks[7]);
        pointer = image + *address_pointer_offset;
    }
    while(bytes_left != 0) {
        if(indirect_index == 64) { //finished all indirect nodes
            printf("In wfs_read, no more indirect indexes\n");
            break;
        }
        if(bytes_that_can_be_read == 0) {
            break;
        }
        if (first_read) { // we may be doing the first read form here, so need to take offset into account
            if(bytes_left < 512 - first_block_offset) {
                quantity = bytes_left;
            } else {
                quantity = 512 - first_block_offset;
            }
            pointer += first_block_offset;
            memcpy(buf, pointer, quantity);
            bytes_left = bytes_left - quantity;
            buf = buf + quantity;
            ++address_pointer_offset;
            pointer = image + *address_pointer_offset;
            first_read = 0;
            bytes_read = bytes_read + quantity;
            continue;
        }
        if(bytes_left >= 512) { //guaranteed to be 512 bytes long
            memcpy(buf, pointer, 512);
            bytes_left -= 512;
            buf += 512;
            ++address_pointer_offset;
            pointer = image + *address_pointer_offset;
            bytes_read += 512;
        } else { //last read
            memcpy(buf, pointer, bytes_left);
            bytes_read += bytes_left;
            bytes_left = 0;
        }
        ++indirect_index;
    }
    return bytes_read;
}

int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    printf("Calling write with size %ld\n", size);
    struct wfs_inode *curr_inode = find_inode(path);
    if(curr_inode == NULL) {
        return -ENOENT;
    }
    // if(!S_ISREG(curr_inode->mode)) {
    //     return -EISDIR;
    // }
    long allocated_memory = (curr_inode->size + 511) & (~511); //how much memory is already alocated for this file
    printf("allocated memory is %ld\n", allocated_memory);
    printf("offset is %ld\n", offset);
    long new_file_end_byte = (long)offset + size; // how much the file wants to extend its contents in memory, if any. Also is the new size
    printf("new_file end byte is %ld\n", new_file_end_byte);
    if(new_file_end_byte > 512 * 7 + 512 * 64) { //checks if file is not becoming too big. If it is, we allow write, but only until limit SUSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSS
        new_file_end_byte = 512 * 7 + 512 * 64;
    }
    long* address_pointer_offset = NULL;
    long new_memory_needed = new_file_end_byte - allocated_memory;
    long returned;
    if(new_memory_needed > 0) { //get new memory
        long used_blocks = 0;
        long indirect_blocks_used = 0;
        for(int i = 0; i < 8; ++i) {
            if(curr_inode->blocks[i] != 0) {
                ++used_blocks;
            }
        }
        if (used_blocks == 8) {//indirect blocks already being used. See how many indirect pointers they already have and set pointer to the first unused one
            address_pointer_offset = (long*)(image + curr_inode->blocks[7]);
            for(int j = 0; j < 64; ++j) {
                if(*address_pointer_offset != 0) {
                    printf("j is %d, value is %ld\n", j, *address_pointer_offset);
                    ++indirect_blocks_used;
                    ++address_pointer_offset;
                }
            }
            address_pointer_offset = ((long*)(image + curr_inode->blocks[7])) + indirect_blocks_used;

        }
        while(new_memory_needed > 0) { //let's allocate all the blocks we need for the write
            if(used_blocks < 7) { //this means that we can still directly allocate the blocks
                returned = get_new_data_block(curr_inode);
                if(returned == -1) {
                    return -ENOSPC; //didn't find a new data block, because it is out of space
                }
                clear_block(image + curr_inode->blocks[returned], 1);
                ++used_blocks;
                new_memory_needed -= 512;
            } else if (used_blocks == 7) { //allocate indirect block
                returned = get_new_data_block(curr_inode);
                if(returned == -1) {
                    return -ENOSPC; //didn't find a new data block, because it is out of space
                    printf("second enospc\n");
                }
                curr_inode->blocks[7] = returned;
                clear_block(image + curr_inode->blocks[7], 1);
                address_pointer_offset = (long*)(image + curr_inode->blocks[7]); //sets the pointer to the newly allocated indirect block
                ++used_blocks;
            } else {
                if(indirect_blocks_used >= 64) {
                    return -ENOSPC; // file is exceeding max file size
                    printf("third enospc\n");
                }
                long returned = get_new_data_block(curr_inode);
                if(returned == -1) {
                    return -ENOSPC; // no more space in system
                    printf("fourth enospc\n");
                }
                clear_block(image + returned, 1);
                *address_pointer_offset = returned;//sets the value of the pointer in this address
                ++address_pointer_offset;//updates the pointer itself
                ++indirect_blocks_used;//updates number of indirect blocks used
                new_memory_needed -= 512;
            }
        }
    }
    long block_number = offset / 512;
    long indirect_index = -1;
    char* pointer;
    long bytes_written = 0;
    int was_only_direct = 0;
    if(block_number > 6) { //if block number is less than 6, setting the initial pointer to the block. If greater, setting it according to indirect block
        indirect_index = block_number - 7;
        address_pointer_offset = ((long*)(image + curr_inode->blocks[7])) + indirect_index;
        pointer = image + *address_pointer_offset;
    } else {
        was_only_direct = 1;
        pointer = image + curr_inode->blocks[block_number];
    }
    long first_block_offset = offset % 512;
    long bytes_left = new_file_end_byte - (long)offset; //size
    int first_write = 1;
    long quantity;
    while(bytes_left != 0) {
        if(block_number >= 7) { //if we reach block 7, we need to go to the next loop for the indirect blocks
            break;
        }
        if(first_write) { //first read, need to consider offset
            pointer += first_block_offset;
            if(bytes_left < 512 - first_block_offset) {
                quantity = bytes_left;
            } else {
                quantity = 512 - first_block_offset;
            }
            memcpy(pointer, buf, quantity);
            bytes_left = bytes_left - quantity;
            buf = buf + quantity;
            block_number++;
            pointer = image + curr_inode->blocks[block_number];
            bytes_written = bytes_written + quantity;
            first_write = 0;
            continue;
        }
        if (bytes_left >= 512) { //subsequent read, can copy whole 512 bytes because first read guarantees it
            memcpy(pointer, buf, 512);
            bytes_left -= 512;
            buf += 512;
            block_number++;
            pointer = image + curr_inode->blocks[block_number];
            bytes_written += 512;
        } else { //last read if we don't need to get into the indirect block
            memcpy(pointer, buf, bytes_left);
            bytes_written += bytes_left;
            bytes_left = 0;
        }
    }
    if(was_only_direct) {//first time using indirect block, need to set the pointer accordingly. 
    //If offset makes us write here first, pointer already set in one of the first ifs
        indirect_index = 0;
        address_pointer_offset = (long*)(image + curr_inode->blocks[7]);
        pointer = image + *address_pointer_offset;
    }
    while(bytes_left != 0) {
        if(indirect_index == 64) { //finished all indirect nodes
            printf("In wfs_read, no more indirect indexes\n");
            break;
        }
        if (first_write) { // we may be doing the first read form here, so need to take offset into account
            if(bytes_left < 512 - first_block_offset) {
                quantity = bytes_left;
            } else {
                quantity = 512 - first_block_offset;
            }
            pointer += first_block_offset;
            memcpy(pointer, buf, quantity);
            bytes_left = bytes_left - quantity;
            buf = buf + quantity;
            ++address_pointer_offset;
            pointer = image + *address_pointer_offset;
            first_write = 0;
            bytes_written = bytes_written + quantity;
            continue;
        }
        if(bytes_left >= 512) { //guaranteed to be 512 bytes long
            memcpy(pointer, buf, 512);
            bytes_left -= 512;
            buf += 512;
            ++address_pointer_offset;
            pointer = image + *address_pointer_offset;
            bytes_written += 512;
        } else { //last read
            memcpy(pointer, buf, bytes_left);
            bytes_written += bytes_left;
            bytes_left = 0;
        }
        ++indirect_index;
    }
    if(curr_inode->size < new_file_end_byte) {
        curr_inode->size = new_file_end_byte;
    }
    printf("finished write. Wrote %ld\n", bytes_written);
    return (int)bytes_written;
}
 
int main (int argc, char* argv[]) {
    char *disk_img = argv[1];
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
    image = (char *)img;
    super = (struct wfs_sb *) image;
    char* fuse_argv[argc - 1];
    int fuse_argc = argc - 1;
    fuse_argv[0] = strdup(argv[0]);
    for (int i = 2; i < fuse_argc + 1; i++) {
        fuse_argv[i - 1] = strdup(argv[i]);
    }

    return fuse_main(fuse_argc, fuse_argv, &ops, NULL);
}
