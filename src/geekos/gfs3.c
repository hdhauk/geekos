/*
 * GeekOS file system
 * Copyright (c) 2008, David H. Hovemeyer <daveho@cs.umd.edu>, 
 * Neil Spring <nspring@cs.umd.edu>, Aaron Schulman <schulman@cs.umd.edu>
 *
 * All rights reserved.
 *
 * This code may not be resdistributed without the permission of the copyright holders.
 * Any student solutions using any of this code base constitute derviced work and may
 * not be redistributed in any form.  This includes (but is not limited to) posting on
 * public forums or web sites, providing copies to (past, present, or future) students
 * enrolled in similar operating systems courses the University of Maryland's CMSC412 course.
 */


#include <limits.h>
#include <geekos/errno.h>
#include <geekos/kassert.h>
#include <geekos/screen.h>
#include <geekos/malloc.h>
#include <geekos/string.h>
#include <geekos/bitset.h>
#include <geekos/synch.h>
#include <geekos/bufcache.h>
#include <geekos/gfs3.h>
#include <geekos/pfat.h>
#include <geekos/projects.h>
#include <geekos/vfs.h>

/* ----------------------------------------------------------------------
 * Private data and functions
 * ---------------------------------------------------------------------- */

struct GFS3_Instance {
    struct gfs3_superblock *superblock;
    ulong_t block_with_root;
    struct FS_Buffer_Cache *fs_buf_cache;
    struct gfs3_inode *root_dir_inode;
    struct gfs3_dirent *root_dirent;
    char *bitmap;
    // TODO
};

struct GFS3_File {
    struct gfs3_inode *inode;
    gfs3_inodenum inodenum;
    char *file_data_cache;
};



char *sprint_dir_name(char *name, unsigned char length){
    char *ret = (char *)Malloc(length);
    strcpy(ret,name);
    ret[length] = '\0';
    return ret;
}

const char *sprint_inode_type( unsigned char type){
    if (type == GFS3_DIRECTORY){
        return "directory";
    } else if (type == GFS3_FILE){
        return "file";
    } else {
        return "invalid type";
    }
}



unsigned int used_extents(struct gfs3_inode *inode){
    int i;
    unsigned int used = 0;
    struct gfs3_extent *extent = NULL;
    for (i = 0; i < 3; i++){
        extent = &inode->extents[i];
        if (extent->length_blocks > 0){
            used ++;
        }
    }
    return used;
}

void print_dirent(struct gfs3_dirent *d){
    Print("================ DIRENT ================\n");
    Print("\tname               = %s\n", sprint_dir_name(d->name, d->name_length));
    Print("\tname_length        = %u\n", d->name_length);
    Print("\tparent inode num   = %u\n", d->inum);
    Print("\tentry_length       = %u\n", d->entry_length);
    Print("========================================\n\n");
}







void print_inode(struct gfs3_inode *inode, gfs3_inodenum num){
    Print("=============== INODE #%d ===============\n", num);
    Print("\tsize          = %u\n", inode->size);
    Print("\ttype          = %u (%s)\n", inode->type, sprint_inode_type(inode->type));
    Print("\trefcount      = %u\n", inode->reference_count);
    Print("\tmode          = %u\n", inode->mode);

    Print("\textents (in use = %u):\n", used_extents(inode));
    int i;
    for (i = 0; i < 3; i++){
        Print("\t\textents[%d] = (start_block = %3u, length (in blocks) = %3u)\n",
              i, inode->extents[i].start_block, inode->extents[i].length_blocks);
    }
    Print("========================================\n\n");
}


ulong_t block_num_root(struct GFS3_Instance *inst){
    return inst->superblock->block_with_inode_zero;
}





struct gfs3_inode *get_inode(struct FS_Buffer_Cache *bc, gfs3_inodenum inodenum){

    ulong_t block_num = BLOCKNUM_FROM_INODENUM(inodenum);
    //Print("\tlooking for inode # %u in block %u with offset %d\n",
    //      inodenum, (unsigned int)block_num, OFFSET_IN_BLOCK(inodenum) );

    struct FS_Buffer *buf;
    /*int n = */Get_FS_Buffer(bc, block_num, &buf);
    //if (n != 0){
    //    Print("\tn = %d\n",n);
    //}

    //Print("\treading with offset = %u\n", OFFSET_IN_BLOCK(inodenum));
    struct gfs3_inode *inode = (struct gfs3_inode *)(buf->data + OFFSET_IN_BLOCK(inodenum));

    Release_FS_Buffer(bc, buf);
    //print_inode(inode, inodenum);
    return inode;
}

struct gfs3_inode * get_root_node(struct GFS3_Instance *inst){
    return get_inode(inst->fs_buf_cache, 1);
    /*
    struct FS_Buffer *buf;
    int n = Get_FS_Buffer(inst->fs_buf_cache, block_num_root(inst), &buf);
    if (n != 0) { Print("\n failed to read block\n"); }

    Release_FS_Buffer(inst->fs_buf_cache, buf);
    return (struct gfs3_inode *)(buf->data + GFS3_INODE_SIZE);
    */
}



unsigned int get_inode_size(struct FS_Buffer_Cache *bc, gfs3_inodenum inodenum){
    struct gfs3_inode *node = get_inode(bc, inodenum);
    return node->size;
}

bool is_dir(struct gfs3_inode * inode){
    //KASSERT(!(inode->type != GFS3_FILE && inode->type != GFS3_DIRECTORY ));

    return inode->type == GFS3_DIRECTORY;
}


bool inode_is_dir(struct FS_Buffer_Cache *bc, gfs3_inodenum inodenum){
    struct gfs3_inode *node = get_inode(bc, inodenum);
    if (node->type != GFS3_FILE && node->type != GFS3_DIRECTORY){
        print_inode(node,inodenum);
    }

    return is_dir(node);
}

struct gfs3_dirent *next(struct gfs3_dirent *d){
    return (struct gfs3_dirent *)((int)d + 4 + d->entry_length);
}

void extract_dirent_name(struct gfs3_dirent *d, char name[251]){
    memcpy(name,d->name, d->name_length);
    name[d->name_length] = '\0';
}


bool file_in_dirent(
        struct FS_Buffer_Cache *bc,
        struct gfs3_dirent *dirent,
        unsigned int size_in_inode,
        char *name,
        gfs3_inodenum *target){
    //Print("\tlooking for: \"%s\"\n", name);

    // read "."
    unsigned int size_seen = 0;
    //gfs3_inodenum parent_inode = dirent->inum;
    //size_seen += (4 + dirent->entry_length);
    //Print("\t\t looked at (%d/%d)bytes\n", size_seen, size_in_inode);


    // read ".."
    struct gfs3_dirent *current = dirent;
    //current = next(dirent);
    //size_seen += (4 + current->entry_length);
    //KASSERT(current->inum == parent_inode);
    //Print("\t\t looked at (%d/%d)bytes\n", size_seen, size_in_inode);


    while(size_seen < size_in_inode){
        //print_dirent(current);



        // read name of entry
        char current_name[251];
        extract_dirent_name(current, current_name);

        // compare name to what we are looking for
        bool found = strncmp(name, current_name, strlen(name)) == 0;
        if (found){
            // Print("\t found file! Looked for \"%s\", found \"%s\"\n",name, current_name);
            *target = current->inum;
            return true;
        }

        // add size of the dirent we just looked at
        size_seen += (4 + current->entry_length);

        // print status
        //struct gfs3_inode *current_inode = get_inode(bc,current->inum);
        //bool dir = is_dir(current_inode);
        // Print("\tfound inode #%u, name: \"%s\"(is dir: %s) [%d/%d]bytes read\n",current->inum,
        //       current_name, BOOL_PRINT(dir), size_seen, size_in_inode);

         // print_dirent(current);
        // go to next dirent
        current = next(current);

    }
    return false;
}




void print_superblock(const struct gfs3_superblock *block){
    Print("============== SUPERBLOCK ==============\n");
    Print("\tmagic              = 0x%x\n", block->gfs3_magic);
    Print("\tversion            = 0x%x\n", block->gfs3_version);
    Print("\tblock with inode 0 = %u\n", block->block_with_inode_zero);
    Print("\tnumber of inodes   = %u\n", block->number_of_inodes);
    Print("\tblocks per disk    = %u\n", block->blocks_per_disk);
    Print("========================================\n\n");
}


struct gfs3_dirent *get_dirent(struct FS_Buffer_Cache *bc, struct gfs3_inode *inode){
    //KASSERT(is_dir(inode));
    if (!is_dir(inode)){ return NULL;}

    // TODO: is this safe?!
    struct gfs3_extent *extent = &inode->extents[0];

    struct FS_Buffer *buf;
    int n = Get_FS_Buffer(bc, extent->start_block, &buf);
    if (n != 0){ return NULL; }

    struct gfs3_dirent *dir = (struct gfs3_dirent *)buf->data;

    Release_FS_Buffer(bc,buf);
    return dir;
}

// return true if ok, false if too long
bool valid_path(const char *path){
    if (strlen(path) > GFS3_MAX_PATH_LEN){
        return false;
    }

    // get a working copy of path that we can peel off
    char mutable_path[GFS3_MAX_PATH_LEN + 1];
    strcpy(mutable_path,path);


    char prefix[GFS3_MAX_PREFIX_LEN + 1]; // make room for \0
    const char *suffix = 0;


    // iterate through path to check individual parts
    do{
    bool valid = Unpack_Path(mutable_path, prefix, &suffix);
    if (!valid){
        return false;
    }

    // peel off prefix
    strncpy(mutable_path,suffix,GFS3_MAX_PATH_LEN+1);
    // Print("prefix = \"%s\"\n", prefix);
    // Print("suffix = \"%s\"\n", suffix);

        // if(strcmp(prefix, "") == 0){
    //     Print("prefix = \"%s\"\n", prefix);
    //     Print("suffix = \"%s\"\n", suffix);
    //     return false;
    // }

    // Print("prefix = \"%s\"\n", prefix);
    // Print("suffix = \"%s\"\n", suffix);

    } while ( strlen(suffix) > 1);

    return true;

}

// return 0 if directory not found
gfs3_inodenum lookup(struct GFS3_Instance *instance, const char *path, struct gfs3_inode **node){
    struct gfs3_inode *root = get_root_node(instance);

    // looking for root
    if (strcmp(path, "/") == 0){
        *node = root;
        return 1;
    }

    // get a copy of path string we can work with.
    char mutable_path[GFS3_MAX_PATH_LEN + 1];
    strcpy(mutable_path,path);

    char prefix[GFS3_MAX_PREFIX_LEN + 1]; // make room for \0
    const char *suffix = 0;
    Unpack_Path(path, prefix, &suffix);
    // Print("prefix = %s\n", prefix);
    // Print("suffix = %s\n", suffix);

    struct gfs3_inode *current_inode = root;
    struct gfs3_dirent *current_dirent;
    gfs3_inodenum target = 1;

//    int i = 0;
    while( suffix[0] == '/' && strlen(suffix) > 1){
        //Print("len(suffix) == %d\n", (int)strlen(suffix));
        //Unpack_Path(path, prefix, &suffix);
        //minFreeEntry = -1;

        // Print("--> looking for prefix %s in inode #%u\n", prefix, target);
        if (!is_dir(current_inode)){
            Print("current inode is not direcotort\n" );
        }
        current_dirent = get_dirent(instance->fs_buf_cache, current_inode);

        bool is_present =
                file_in_dirent(instance->fs_buf_cache, current_dirent, current_inode->size, prefix, &target);
        if (!is_present){
            // Print("\t did not find file or directory...\n");
            *node = NULL;
            return 0;
        }

        // found the prefix
        // Print("--> prefix \"%s\" found. Its inode is %u\n", prefix, target);


        current_inode = get_inode(instance->fs_buf_cache, target);
        is_dir(current_inode);


        strncpy(mutable_path,suffix,GFS3_MAX_PATH_LEN+1);
        Unpack_Path(mutable_path, prefix, &suffix);
    }


    // prefix now hold last part of path and current_inode is the inode of the final directory
    struct gfs3_dirent *final_dirent = get_dirent(instance->fs_buf_cache, current_inode);
    gfs3_inodenum file_inodenum;
    bool file_found =
            file_in_dirent(instance->fs_buf_cache, final_dirent, current_inode->size,prefix,&file_inodenum);
    if (!file_found){
        *node = current_inode;
        Print("found directory, but not file in that directory...\n");
        return 0;
    }

    // get
    *node = get_inode(instance->fs_buf_cache, file_inodenum);
    return file_inodenum;
}






void get_first_blocks(struct gfs3_inode *inode,
                         gfs3_blocknum *start_block, unsigned int *size_in_blocks){
    struct gfs3_extent *extent = &inode->extents[0];
    *start_block = extent->start_block;
    *size_in_blocks = extent->length_blocks;
}


gfs3_inodenum next_unused_inode(struct GFS3_Instance * inst){
    gfs3_inodenum i;
    struct gfs3_inode *node;

    for(i = 1; i < 50; i++){
        node = get_inode(inst->fs_buf_cache, i);
        //print_inode(node, i);

        bool is_file = node->type == GFS3_FILE;
        bool is_dir = node->type == GFS3_DIRECTORY;
        bool is_free = !(is_file || is_dir );
         if (is_free){
             return i;
         }

    }
    return 0;
}

struct gfs3_inode *init_file_inode(struct GFS3_Instance *inst, gfs3_inodenum inum, unsigned short mode){

    // make inode
    struct gfs3_inode *inode = (struct gfs3_inode *)Malloc(sizeof(struct gfs3_inode));
    memset(inode, '\0', sizeof(struct gfs3_inode));
    inode->type = GFS3_FILE;
    inode->mode = mode;
    inode->reference_count = 1;
    inode->size = 0;

    struct FS_Buffer *buf;
    Print("initing inode %d in block %d at offset %d\n",(int)inum,
          (int)BLOCKNUM_FROM_INODENUM(inum), (int)OFFSET_IN_BLOCK(inum));
    int rc = Get_FS_Buffer(inst->fs_buf_cache, BLOCKNUM_FROM_INODENUM(inum), &buf);
    if (rc != 0){
        Print("\trc = %d\n",rc);
    }
    // copy inode to buffer
    Modify_FS_Buffer(inst->fs_buf_cache, buf);
    memcpy(buf->data + OFFSET_IN_BLOCK(inum), inode, sizeof(struct gfs3_inode));

    //Sync_FS_Buffer_Cache(inst->fs_buf_cache);
    //Print("all good so far...\n");
    Release_FS_Buffer(inst->fs_buf_cache, buf);

    return inode;
}


// return false if pos is outside all extents
bool pos_in_extents(struct GFS3_File *gfile, ulong_t pos, ulong_t *ext_id, ulong_t *free_in_ext){
    if (gfile->inode->extents[0].length_blocks == 0){
        *ext_id = 0;
        *free_in_ext = 0;
        return false;
    }
    ulong_t size_ext0 = gfile->inode->extents[0].length_blocks * GFS3_BLOCK_SIZE;
    if (size_ext0 > pos){
        // position is in the first extent
        *ext_id = 0;
        *free_in_ext = size_ext0 - pos;
        return true;
    }

    if (gfile->inode->extents[1].length_blocks == 0){
        *ext_id = 1;
        *free_in_ext = 0;
        return false;
    }
    ulong_t size_ext1 = gfile->inode->extents[1].length_blocks * GFS3_BLOCK_SIZE;
    if (size_ext1 > (pos - size_ext0)){
        // position is in second extent
        *ext_id = 1;
        *free_in_ext = size_ext1 - (pos - size_ext0);
        return true;
    }

    if (gfile->inode->extents[2].length_blocks == 0){
        *ext_id = 2;
        *free_in_ext = 0;
        return false;
    }
    ulong_t size_ext2 = gfile->inode->extents[2].length_blocks * GFS3_BLOCK_SIZE;
    if (size_ext2 > (pos - size_ext0 - size_ext1)){
        *ext_id = 2;
        *free_in_ext = size_ext2 - (pos - size_ext0 - size_ext1);
        return true;
    }

    return false;

}

// -1 if no free
int id_of_next_free_extent(struct gfs3_inode *inode){
    if (inode->extents[0].length_blocks == 0){
        return 0;
    }
    else if (inode->extents[1].length_blocks == 0){
        return 1;
    }
    else if (inode->extents[2].length_blocks == 0){
        return 2;
    }
    return -1;
}







///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
struct GFS3_File *Get_GFS3_File(struct GFS3_Instance *instance, struct gfs3_inode *inode,
                                gfs3_inodenum inodenum){
    struct GFS3_File *file = 0;
    char *file_data_cache = 0;


    gfs3_blocknum start_blk;
    unsigned int num_of_blks;
    get_first_blocks(inode,&start_blk,&num_of_blks);

    // Print("start block: %u, number of blocks %u\n", start_blk, num_of_blks);

    file = (struct GFS3_File *)Malloc(sizeof(*file));
    if (file == 0){
        goto memfail;
    }

    file_data_cache = (char *)Malloc(GFS3_BLOCK_SIZE);
    if (file_data_cache == 0){
        goto memfail;
    }

    file->inode = inode;
    file->inodenum = inodenum;
    file->file_data_cache = file_data_cache;


    goto done;

    memfail:
        if (file != 0){
            Free(file);
        }
        if (file_data_cache != 0){
            Free(file_data_cache);
        }
        file = NULL;

    done:
        return file;
}

void get_start_block(struct gfs3_extent *extent, gfs3_blocknum *start, gfs3_blocknum *length){
    *start = extent->start_block;
    *length = extent->length_blocks;
}

struct gfs3_extent *get_extent(struct gfs3_inode *inode, unsigned int num){
    KASSERT(num < 4);
    return &inode->extents[num];
}

bool has_data(struct gfs3_extent *extent){
    return extent->length_blocks > 0;
}

ulong_t get_extent_num(struct gfs3_inode *inode, ulong_t file_pos, gfs3_blocknum *pos_block){

    gfs3_blocknum sequential_block = file_pos / GFS3_BLOCK_SIZE;
    Print("sequential block = %d\n", sequential_block);

    gfs3_blocknum total = 0;
    // check in extent 0
    struct gfs3_extent *ext0 = get_extent(inode, 0);
    gfs3_blocknum start0 = 0;
    gfs3_blocknum len0 = 0;

    get_start_block(ext0, &start0,&len0);
    if (len0 > sequential_block){
        *pos_block = start0 + sequential_block;
        return 0;
    }
    total += len0;

    // check extent 1
    struct gfs3_extent *ext1 = get_extent(inode, 1);
    gfs3_blocknum start1 = 0;
    gfs3_blocknum len1 = 0;
    get_start_block(ext1, &start1,&len1);
    total += len1;
    if (total > sequential_block){
        *pos_block = start1 + sequential_block - len0;
        return 1;
    }

    // check extent 2
    struct gfs3_extent *ext2 = get_extent(inode, 2);
    gfs3_blocknum start2 = 0;
    gfs3_blocknum len2 = 0;
    get_start_block(ext2, &start2,&len2);
    total += len2;
    if (total > sequential_block){
        *pos_block = start2 + sequential_block - len1;
        return 2;
    }


    KASSERT(false);

}


/* ----------------------------------------------------------------------
 * Implementation of VFS operations
 * ---------------------------------------------------------------------- */

/*
 * Get metadata for given file.
 */
static int GFS3_FStat(struct File *file, struct VFS_File_Stat *stat) {


    //stat =

    struct GFS3_File *gfs3_file = (struct GFS3_File *)file->fsData;
    struct gfs3_inode *inode = gfs3_file->inode;

    //print_inode(inode, gfs3_file->inodenum);


    stat->size = inode->size;
    //Print("size = %d\n", stat->size);
    stat->isDirectory = (unsigned int)is_dir(inode);
    stat->isSetuid = 0; // TODO: Where can i get this?!

    return 0;


    TODO_P(PROJECT_GFS3, "GeekOS filesystem FStat operation");
    return 0;
}

/*
 * Read data from current position in file.
 */
static int GFS3_Read(struct File *file, void *buf, ulong_t numBytes) {

    struct GFS3_File *gfs3_file = (struct GFS3_File * )file->fsData;
    //struct GFS3_Instance *instance = (struct GFS3_Instance *)file->mountPoint->fsData;

    // print_inode(gfs3_file->inode,gfs3_file->inodenum);

    ulong_t start, end;
    ulong_t i;

    start = file->filePos;
    end = file->filePos + numBytes;

    if (is_dir(gfs3_file->inode)){
        return 0;
        return EINVALID;
    }

    if (end > file->endPos){
        numBytes = file->endPos - file->filePos;
        end = file->endPos;
    }

    // Make sure request represents a valid range within the file
    if(start >= file->endPos || end > file->endPos || end < start) {
        return EINVALID;
    }


    gfs3_blocknum start_block = 0;
    get_extent_num(gfs3_file->inode, file->filePos, &start_block);

    gfs3_blocknum end_block = 0;
    get_extent_num(gfs3_file->inode, file->filePos + numBytes, &end_block);
    // Print("read from filepos %d with size %d\n", (int)file->filePos, (int)numBytes);

    // Print("starting to read from block #%d -->#%d\n", start_block, end_block);

   //  Print("start_block = %d, end_block = %d\n", start_block, end_block);

    gfs3_blocknum first_block = gfs3_file->inode->extents[0].start_block;

    // TODO: this will not work if we're in a second or third extent.
    ulong_t start_offset = start - (start_block -first_block) * GFS3_BLOCK_SIZE;
    // Print("start_offset = %d\n", (int)start_offset);

    gfs3_blocknum current_block = start_block;
    ulong_t num_bytes_read = 0;

    for (i = start_block; i <= end_block; i++){
        int rc;

        rc = Block_Read(file->mountPoint->dev, current_block, gfs3_file->file_data_cache);
        if (rc != 0) {
            Print("Error while reading block\n");
            return EIO;
        }


        if (i == start_block){
            // only copy parts of the first block
            //Print("Reading from start block\n");

            if (numBytes < GFS3_BLOCK_SIZE - start_offset){
                // reading less than to end one block
                // Print("\tReading less than to end of one block\n");
                // KASSERT(end_block == i + 1);
                memcpy(buf,gfs3_file->file_data_cache + start_offset, numBytes);
                num_bytes_read += numBytes;
            } else {
                // Print("\tReading less more than one block\n");

                // KASSERT(end_block >= i+1);
                memcpy(buf, gfs3_file->file_data_cache + start_offset, GFS3_BLOCK_SIZE - start_offset);
                num_bytes_read +=  GFS3_BLOCK_SIZE - start_offset;
            }
        } else if (i == end_block -1) {
            // last block to read from
            ulong_t to_read = numBytes-num_bytes_read;
            memcpy(buf + num_bytes_read, gfs3_file->file_data_cache, to_read);
            num_bytes_read += to_read;
        } else {
            // blocks in between
            // Print("Reading from block in the middle\n");

            memcpy(buf+num_bytes_read, gfs3_file->file_data_cache, GFS3_BLOCK_SIZE);
            num_bytes_read += GFS3_BLOCK_SIZE;
        }
    }

    // update file location
    file->filePos += num_bytes_read;
    // Print("bytes read (%d/%d)  [actually read / supposed to read]\n", (int)num_bytes_read, (int)numBytes);
    KASSERT(num_bytes_read == numBytes);
    // Print("successfully read %d bytes\n", (int)numBytes);
    return (int) num_bytes_read;






    TODO_P(PROJECT_GFS3, "GeekOS filesystem read operation");

    return EUNSUPPORTED;
}

/*
 * Write data to current position in file.
 */
static int GFS3_Write(struct File *file, void *buf, ulong_t numBytes) {

    //ulong_t start_blk, end_blk, current_blk;
    //start_blk = Round_Down_To_Block(file->filePos);
    //end_blk = Round_Up_To_Block(file->endPos);
    //return 0;

    struct GFS3_Instance *instance = (struct GFS3_Instance *)file->mountPoint->fsData;
    struct GFS3_File *gfs3_file = (struct GFS3_File *)file->fsData;
    Print("############# WRITE %d bytes ##################\n", (int)numBytes);
    Print("inode before write:\n");
    print_inode(gfs3_file->inode, gfs3_file->inodenum);

    Print("want to write from pos %d --> %d\n",
          (int)file->filePos, (int)(file->filePos + numBytes));





    bool need_to_increase_size = file->filePos + numBytes > file->endPos;
    if (!need_to_increase_size){
        Print("writing in the middle of the file \n");
        /*
        ulong_t file_pos_blk = Round_Down_To_Block(file->filePos);
        Print("file_pos_blk = %d\n", (int)file_pos_blk);

        uint_t blks_in_ext0 = gfs3_file->inode->extents[0].length_blocks;
        uint_t blks_in_ext1 = gfs3_file->inode->extents[1].length_blocks;
        uint_t blks_in_ext2 = gfs3_file->inode->extents[2].length_blocks;

        bool in_1st_extent =  blks_in_ext0 > file_pos_blk;
        bool in_2nd_extent = blks_in_ext0 + blks_in_ext1 > file_pos_blk;

        if (in_1st_extent){
            gfs3_blocknum start_ext0 = gfs3_file->inode->extents[0].start_block;

            struct FS_Buffer *fBuf;
            Get_FS_Buffer(instance->fs_buf_cache, start_ext0 + file_pos_blk, &fBuf);

            ulong_t offset_in_blk = file->filePos - (Round_Down_To_Block(file->filePos) * GFS3_BLOCK_SIZE);

            Print("starting writing in extent[0] with blk_offset %d and byte offset %d\n",
                  (int)file_pos_blk, (int)offset_in_blk);

            memcpy(fBuf + file_pos_blk*GFS3_BLOCK_SIZE +  offset_in_blk, buf,numBytes);
            Release_FS_Buffer(instance->fs_buf_cache, fBuf);
            return (int) numBytes;

        } else if (in_2nd_extent){

        } else {

        }


    */

        return (int) numBytes;
    }

    ulong_t id, free_in_ext;
    bool starting_in_existing_extent = pos_in_extents(gfs3_file, file->filePos, &id, &free_in_ext);


    bool fits_in_existing_extent = (numBytes <= free_in_ext) && starting_in_existing_extent;
    if (fits_in_existing_extent){
        Print("extent id = %d, free in extent %d\n", (int)id, (int) free_in_ext);
        // trivial write...
        Print("fits in current extent[%d]. Just have to append to it\n", (int)id);
        struct FS_Buffer *fBuf;
        Get_FS_Buffer(instance->fs_buf_cache, gfs3_file->inode->extents[id].start_block, &fBuf);
        Modify_FS_Buffer(instance->fs_buf_cache, fBuf);
        memcpy(fBuf->data + file->filePos,buf, numBytes);
        Release_FS_Buffer(instance->fs_buf_cache, fBuf);

        goto cleanup;
    }




    Print("Doesn't fit in current extents...Have to alloc some...\n");
    ulong_t extra_needed_space = (numBytes-free_in_ext);
    ulong_t blks_needed = 1 + extra_needed_space / GFS3_BLOCK_SIZE;
    Print("We need %d extra blocks. Can stuff %d bytes in existing blocks, but need room for %d\n",
          (int)blks_needed, (int)free_in_ext, (int)extra_needed_space);
    uint_t free_blk = (uint_t)Find_First_N_Free(instance->bitmap, (uint_t)blks_needed, GFS3_BITMAP_SIZE);
    Print("next free block turned out to be #%d\n", (int)free_blk);


    unsigned int last_blk = gfs3_file->inode->extents[id].start_block + gfs3_file->inode->extents[id].length_blocks;
    Print("last block in extent[%d] is %d\n", (int)id, (int)last_blk);
    bool free_space_beyond_extent = free_blk == last_blk;
    if (free_space_beyond_extent){
        Print("free space after current extent. expanding!\n");

        // extend extent
        gfs3_file->inode->extents[id].length_blocks += (uint_t)blks_needed;

        // updating bitmap
        int i;
        for(i=0; i < (int)blks_needed; i++){
            Print("setting block %d as occupied\n", free_blk + i);
            Set_Bit(instance->bitmap, free_blk + i);
        }

        // write from last pos
        struct FS_Buffer *fBuf;
        Get_FS_Buffer(instance->fs_buf_cache, gfs3_file->inode->extents[id].start_block, &fBuf);
        Modify_FS_Buffer(instance->fs_buf_cache, fBuf);
        memcpy(fBuf->data + file->filePos, buf, numBytes);
        Release_FS_Buffer(instance->fs_buf_cache, fBuf);
        goto cleanup;



        return (int)numBytes;
    }


    bool free_extent = true;
    if (free_extent){
        int ext_id = id_of_next_free_extent(gfs3_file->inode);
        Print("We have a free extent(%d)!\n",ext_id);
        // fill up current extent
        // allocate new extent & update in bitmap
        int i;
        for(i=0; i < (int)blks_needed; i++){
            Print("setting block %d as occupied\n", free_blk + i);
            Set_Bit(instance->bitmap, free_blk + i);
        }


        // save new extent
        gfs3_file->inode->extents[ext_id].start_block= free_blk;
        gfs3_file->inode->extents[ext_id].length_blocks = (uint_t)blks_needed;
        struct FS_Buffer *fBuf;
        Get_FS_Buffer(instance->fs_buf_cache, free_blk, &fBuf);
        Modify_FS_Buffer(instance->fs_buf_cache, fBuf);
        memcpy(fBuf->data, buf, numBytes);
        Release_FS_Buffer(instance->fs_buf_cache, fBuf);
        goto cleanup;

        // write rest to new extent
        return (int)numBytes;
    }

    // must coaless
    // TODO
    return ENOSPACE;



    cleanup:
        file->filePos += numBytes;
        gfs3_file->inode->size += (unsigned int)numBytes;
        if (file->filePos > file->endPos){
            file->endPos = file->filePos;
        }


        return (int)numBytes;


}


/*
 * Seek to a position in file; returns 0 on success.
 */
static int GFS3_Seek(struct File *file, ulong_t pos) {

    /*
     * Similar errors as Read. Acts as unix seek, assuming SEEK SET,
     * that is, an absolute position is the offset. The behavior of
     * seek() on gfs3 directories shall be undefined; the behavior
     * of readdir after seek equally undefined. (Undefined means do
     * what you want; I don’t want to specify.)
     *
     *
     * Read()
     * Acts as unix read(). Returns EACCESS if file was not opened
     * for reading. EINVALID if fd out of range, ENOMEM if no memory,
     * ENOTFOUND if the file descriptor has not been opened or has
     * been closed. It’s fine to return fewer bytes than asked for,
     * for example, when near the end of the file. Be sure to advance
     * the file position.
     * */


    // TODO: What else....??
    file->filePos = pos;
    return 0;


    TODO_P(PROJECT_GFS3, "GeekOS filesystem seek operation");
    return EUNSUPPORTED;
}

/*
 * Close a file.
 */
static int GFS3_Close(struct File *file) {



    TODO_P(PROJECT_GFS3, "GeekOS filesystem close operation");
    return EUNSUPPORTED;
}

/*static*/ struct File_Ops s_gfs3FileOps = {
    &GFS3_FStat,
    &GFS3_Read,
    &GFS3_Write,
    &GFS3_Seek,
    &GFS3_Close,
    0,                          /* Read_Entry */
};

/*
 * Stat operation for an already open directory.
 */
static int GFS3_FStat_Directory(struct File *dir,
                                struct VFS_File_Stat *stat) {
    /* may be unused. */

    return GFS3_FStat(dir,stat);
    TODO_P(PROJECT_GFS3, "GeekOS filesystem FStat directory operation");
    return 0;
}

/*
 * Directory Close operation.
 */
static int GFS3_Close_Directory(struct File *dir) {



    TODO_P(PROJECT_GFS3, "GeekOS filesystem Close directory operation");
    return EUNSUPPORTED;
}




/*
 * Read a directory entry from an open directory.
 */
static int GFS3_Read_Entry(struct File *dir, struct VFS_Dir_Entry *entry) {

    //Print("********************* START *********************\n");
    struct GFS3_File *gfs3_file = (struct GFS3_File *)dir->fsData;
    struct GFS3_Instance *instance = dir->mountPoint->fsData;
    //print_inode(gfs3_file->inode, gfs3_file->inodenum);

    struct gfs3_dirent *dirent = get_dirent(instance->fs_buf_cache,gfs3_file->inode);

    ulong_t i;
    for (i = 0; i < dir->filePos; i++){
        dirent = next(dirent);
    }
    // print_dirent(dirent);
    // Print("Retrieving element #%d in directory\n", (int)i);
    // Print("That would be inode #%d\n", dirent->inum);




    struct gfs3_inode *entry_inode = get_inode(instance->fs_buf_cache,dirent->inum);

    dir->filePos++;



    const char *dirname = sprint_dir_name(dirent->name,dirent->name_length);
    strcpy(entry->name, dirname);
    //Print("entry->name = \"%s\"\n", entry->name);


    struct VFS_File_Stat *stat = (struct VFS_File_Stat *)Malloc(sizeof(struct VFS_File_Stat));
    stat->isDirectory = (unsigned int)is_dir(entry_inode);
    //Print("\"%s\"is directory = %d\n",entry->name, stat->isDirectory);
    stat->isSetuid = 0;
    stat->size = entry_inode->size;
    memcpy(&entry->stats, stat, sizeof(struct VFS_File_Stat));
    Free(stat);

    return 0;

    /*
    directoryEntry *directory;
    directoryEntry *gfs3_dir_entry;

    bool reached_end_of_dir = dir->filePos >= dir->endPos;
    if (reached_end_of_dir){
        return VFS_NO_MORE_DIR_ENTRIES;
    }

    directory = (directoryEntry *)dir->fsData;
    gfs3_dir_entry = &directory[dir->filePos++];

    strncpy(entry->name, gfs3_dir_entry->fileName, sizeof(gfs3_dir_entry->fileName));
    entry->name[sizeof(gfs3_dir_entry->fileName)] = '\0';

    //Copy_Stat(&entry->stats, gfs3_dir_entry);

    struct VFS_File_Stat *stat = (struct VFS_File_Stat *)Malloc(sizeof(struct VFS_File_Stat));

    GFS3_FStat(dir,stat);
    memcpy(&entry->stats, stat, sizeof(struct VFS_File_Stat));
    Free(stat);

    Print("entry->name = \"%s\"\n",entry->name);
    */



    TODO_P(PROJECT_GFS3, "GeekOS filesystem Read_Entry operation");
    return EUNSUPPORTED;
}

/*static*/ struct File_Ops s_gfs3DirOps = {
    &GFS3_FStat_Directory,
    0,                          /* Read */
    0,                          /* Write */
    0,                          /* Seek */
    &GFS3_Close_Directory,
    &GFS3_Read_Entry,
};


/*
 * Open a file named by given path.
 */
static int GFS3_Open(struct Mount_Point *mountPoint, const char *path,
                     int mode, struct File **pFile) {

    // verify path validity
    bool valid = valid_path(path);
    if (!valid){
        return ENAMETOOLONG;
    }
    //Print("path valid: %s\n", BOOL_PRINT(valid));

    // get instance
    struct GFS3_Instance *instance = (struct GFS3_Instance *)mountPoint->fsData;

    // lookup path
    struct gfs3_inode *file_inode;
    gfs3_inodenum node_num= lookup(instance, path, &file_inode);
    if (node_num != 0){
        // Print("*** Lookup Successful ***\n");
        // print_inode(file_inode,node_num);
    }else{
        Print("!!! Lookup UNSUCCESSFUL !!!\n");
        if(mode & O_CREATE){
            // check if direcotry exist
            bool not_even_dir_found = file_inode == NULL;
            if (not_even_dir_found){
                return ENOTFOUND;
            }





            // Print("Should make this inode...\n");
            gfs3_inodenum free = next_unused_inode(instance);
            // Print("next free inode #%d\n", free);
            node_num = free;
            file_inode = init_file_inode(instance, free, mode);


            // add file to directory
            char prefix[GFS3_MAX_PREFIX_LEN + 1]; // make room for \0
            const char *suffix = 0;
            Unpack_Path(path, prefix, &suffix);
            Print("CREATING FILE \"%s\" \n", suffix);

            // create dirent
            //Print("prefix = \"%s\" with size %d\n", prefix, (int)strlen(prefix));
            unsigned char name_len = (unsigned char)strlen(prefix) +(unsigned char)1;
            unsigned char padding = (unsigned  char)4 - (name_len % (unsigned char)4);
            //Print("padding = %u\n", padding);
            unsigned char entry_len = name_len + padding;

            unsigned int dirent_size = 4 + entry_len;
            struct gfs3_dirent *dirent = (struct gfs3_dirent *)Malloc(dirent_size);
            dirent->name_length = name_len;
            dirent->entry_length = entry_len;
            strncpy(dirent->name, prefix, name_len);

            struct gfs3_inode *dir = file_inode;
            gfs3_inodenum dir_num = lookup(instance,prefix, &dir);
            //print_inode(dir, dir_num);
            dirent->inum = free;
            //print_dirent(dirent);

            // insert dirent
            struct FS_Buffer *buffer;
            Get_FS_Buffer(instance->fs_buf_cache, dir->extents[0].start_block, &buffer);
            memcpy(buffer->data + dir->size, dirent, dirent_size);
            Modify_FS_Buffer(instance->fs_buf_cache, buffer);
            Release_FS_Buffer(instance->fs_buf_cache, buffer);

            // update size of inode
            // Print("THE ONE WE GET FROM get_inode():\n");
            struct gfs3_inode *parent_dir = get_inode(instance->fs_buf_cache, dir_num);
            // print_inode(parent_dir, dir_num);
            Get_FS_Buffer(instance->fs_buf_cache, BLOCKNUM_FROM_INODENUM(dir_num), &buffer);
            Modify_FS_Buffer(instance->fs_buf_cache, buffer);
            parent_dir = buffer->data + OFFSET_IN_BLOCK(dir_num);
            // Print("From buffer->data:\n");
            // print_inode(parent_dir, dir_num);
            parent_dir->size += dirent_size;
            // print_inode(parent_dir, dir_num);
            Release_FS_Buffer(instance->fs_buf_cache, buffer);

            // print_inode(dir, dir_num);

            // Print("lookup after creating and inserting...\n");
            // struct gfs3_inode *t;
            // gfs3_inodenum t_num = lookup(instance,path, &t);
            // print_inode(t,t_num);



            //Print("inode #%d made\n", free);
        } else{
        return ENOTFOUND;
        }
    }

    if (is_dir(file_inode)){
        // Print("this is not a file!\n");
        return ENOTFOUND;
    }


    struct GFS3_File *gfs3_file = Get_GFS3_File(instance, file_inode, node_num);
    if (gfs3_file == 0){
        return ENOMEM;
    }


    struct File *file =
            Allocate_File(&s_gfs3FileOps,0,file_inode->size,gfs3_file, mode, mountPoint);
    if (file == 0){
        return ENOMEM;
    }

    //file->endPos = file_inode->size;
    //print_inode(gfs3_file->inode, gfs3_file->inodenum);


    *pFile = file;


    //struct gfs3_inode *tst_inode = get_inode(instance->fs_buf_cache, gfs3_file->inodenum);
    //print_inode(tst_inode, gfs3_file->inodenum);

    return 0;

    // TODO_P(PROJECT_GFS3, "GeekOS filesystem open operation");
    return EUNSUPPORTED;
}

/*
 * Create a directory named by given path.
 */
static int GFS3_Create_Directory(struct Mount_Point *mountPoint,
                                 const char *path) {

    Print("GFS3_Create_Directory\n");
    struct GFS3_Instance *instance = mountPoint->fsData;

    char prefix[GFS3_MAX_PREFIX_LEN + 1]; // make room for \0
    const char *suffix = 0;
    Unpack_Path(path, prefix, &suffix);
    Print("suffix = \"%s\", prefix = \"%s\"\n", suffix, prefix);

    struct gfs3_inode *parent;
    gfs3_inodenum parent_num = lookup(instance, prefix, &parent);
    if (!parent || parent_num == 0){
        Print("cannot find path %s\n", prefix);
        return ENOTFOUND;
    }

    Print("Should make this inode...\n");
    gfs3_inodenum dir_num = next_unused_inode(instance);
    struct gfs3_inode *dir = init_file_inode(instance, dir_num, 0);
    dir->type = GFS3_DIRECTORY;

    // create "."
    size_t dot_size = 4 + 4; // 4 for inodenum, 2 lengths, 1 for name, 1 padding
    struct gfs3_dirent *dot = (struct gfs3_dirent *)Malloc(dot_size);
    dot->entry_length = 4;
    dot->inum = dir_num;
    dot->name_length = 1;
    memset(dot->name, '.', 1);
    print_dirent(dot);

    // create ".."
    size_t dotdot_size = 4 + 4; // 4 for inodenum, 2 lengths, 1 for name, 1 padding
    struct gfs3_dirent *dotdot = (struct gfs3_dirent *)Malloc(dotdot_size);
    dotdot->entry_length = 4;
    dotdot->inum = parent_num;
    dotdot->name_length = 2;
    memset(dotdot->name, '.', 2);
    print_dirent(dotdot);

    // find and allocate blocks
    gfs3_blocknum free_blk = (gfs3_blocknum)Find_First_Free_Bit(instance->bitmap, GFS3_BITMAP_SIZE);
    Set_Bit(instance->bitmap, free_blk);

    // write dirents
    struct FS_Buffer *buf;
    Get_FS_Buffer(instance->fs_buf_cache, free_blk, &buf);
    Modify_FS_Buffer(instance->fs_buf_cache, buf);
    memcpy(buf->data, dot, dot_size);
    memcpy(buf->data+dot_size, dotdot, dotdot_size);
    Release_FS_Buffer(instance->fs_buf_cache, buf);

    dir->extents[0].start_block = free_blk;
    dir->extents[0].length_blocks = 1;
    dir->size = (unsigned int) (dot_size + dotdot_size);

    print_inode(dir, dir_num);

    // write inode
    Get_FS_Buffer(instance->fs_buf_cache, BLOCKNUM_FROM_INODENUM(dir_num), &buf);
    Modify_FS_Buffer(instance->fs_buf_cache, buf);
    memcpy(buf->data + OFFSET_IN_BLOCK(dir_num), dir, sizeof(struct gfs3_inode));
    Release_FS_Buffer(instance->fs_buf_cache, buf);

    // add to parent directory
    //Print("prefix = \"%s\" with size %d\n", prefix, (int)strlen(prefix));
    unsigned char name_len = (unsigned char)strlen(prefix) +(unsigned char)1;
    unsigned char padding = (unsigned  char)4 - (name_len % (unsigned char)4);
    //Print("padding = %u\n", padding);
    unsigned char entry_len = name_len + padding;

    unsigned int dirent_size = 4 + entry_len;
    struct gfs3_dirent *dir_dirent = (struct gfs3_dirent *)Malloc(dirent_size);
    dir_dirent->name_length = name_len;
    dir_dirent->entry_length = entry_len;
    strncpy(dir_dirent->name, prefix, name_len);
    dir_dirent->inum = dir_num;
    print_dirent(dir_dirent);

    // update size of parent directory
    Get_FS_Buffer(instance->fs_buf_cache, BLOCKNUM_FROM_INODENUM(parent_num), &buf);
    Modify_FS_Buffer(instance->fs_buf_cache, buf);
    parent = buf->data + OFFSET_IN_BLOCK(parent_num);
    gfs3_blocknum parent_ext0 = parent->extents[0].start_block;
    size_t old_size_parent = parent->size;
    parent->size += dirent_size;
    Release_FS_Buffer(instance->fs_buf_cache, buf);

    Get_FS_Buffer(instance->fs_buf_cache, parent_ext0, &buf);
    Modify_FS_Buffer(instance->fs_buf_cache, buf);
    memcpy(buf->data + old_size_parent, dir_dirent, dirent_size);
    Release_FS_Buffer(instance->fs_buf_cache, buf);

    /*
    struct gfs3_inode *dir = file_inode;
    gfs3_inodenum dir_num = lookup(instance,prefix, &dir);
    //print_inode(dir, dir_num);
    dir_dirent->inum = free;
    //print_dir_dirent(dir_dirent);

    // insert dir_dirent
    struct FS_Buffer *buffer;
    Get_FS_Buffer(instance->fs_buf_cache, dir->extents[0].start_block, &buffer);
    memcpy(buffer->data + dir->size, dir_dirent, dirent_size);
    Modify_FS_Buffer(instance->fs_buf_cache, buffer);
    Release_FS_Buffer(instance->fs_buf_cache, buffer);

    // update size of inode
    dir->size += dirent_size;
    */





    return 0;
    TODO_P(PROJECT_GFS3, "GeekOS filesystem create directory operation");
    return EUNSUPPORTED;
}

/*
 * Open a directory named by given path.
 */
static int GFS3_Open_Directory(struct Mount_Point *mountPoint,
                               const char *path, struct File **pDir) {


    Print("Open directory\n");
    // verify path validity
    bool valid = valid_path(path);
    if (!valid){
        return ENAMETOOLONG;
    }
    //Print("path valid: %s\n", BOOL_PRINT(valid));

    // get instance
    struct GFS3_Instance *instance = (struct GFS3_Instance *)mountPoint->fsData;

    // lookup path
    struct gfs3_inode *dir_inode ;
    gfs3_inodenum node_num= lookup(instance, path, &dir_inode);
    if (node_num != 0){
        // Print("*** Lookup Successful ***\n");
        // print_inode(dir_inode,node_num);
    }else{
        // Print("!!! Lookup UNSUCCESSFUL !!!\n");
        return ENOTFOUND;
    }

    if (!is_dir(dir_inode)){
        // Print("this is not a file!\n");
        return ENOTDIR;
    }

    //print_inode(dir_inode, node_num);

    struct GFS3_File *gfs3_file = Get_GFS3_File(instance, dir_inode, node_num);
    if (gfs3_file == 0){
        return ENOMEM;
    }


    struct File *dir =
            Allocate_File(&s_gfs3DirOps,0,dir_inode->size,gfs3_file, O_READ, mountPoint);
    if (dir == 0){
        return ENOMEM;
    }


    *pDir = dir;


    return 0;




    TODO_P(PROJECT_GFS3, "GeekOS filesystem open directory operation");
    return EUNSUPPORTED;
}

/*
 * Open a directory named by given path.
 */
static int GFS3_Delete(struct Mount_Point *mountPoint, const char *path,
                       bool recursive) {

    Print("deleting %s...\n", path);
    struct GFS3_Instance *instance = mountPoint->fsData;
    struct gfs3_inode *inode = 0;

    gfs3_inodenum num = lookup(instance, path, &inode);
    if (!inode || num == 0){
        Print("cannot delete what cannot be found\n");
        return ENOTFOUND;
    }


    if (is_dir(inode)){
        Print("### Whe are deleting a directory here...\n");
        print_inode(inode, num);
        if (inode ->size > 16){
            Print("cannot delete a non-empty directory \n");
            return EINVALID;
        }
    }



    //print_inode(inode, num);
    struct FS_Buffer *buf;


    // remove from directory
    char prefix[GFS3_MAX_PREFIX_LEN + 1]; // make room for \0
    const char *suffix = 0;
    Unpack_Path(path, prefix, &suffix);
    Print("prefix = \"%s\" with size %d\n", prefix, (int)strlen(prefix));
    Print("suffix = \"%s\" with size %d\n", suffix, (int)strlen(suffix));

    struct gfs3_inode *dir;
    gfs3_inodenum dir_num = lookup(instance, suffix, &dir);

    struct gfs3_dirent *dirent= get_dirent(instance->fs_buf_cache, dir);
    unsigned int seen = 0;
    while (seen < dir->size){
        if (dirent->inum == num){
            print_dirent(dirent);
        }
        seen += dirent->entry_length + 4;
        dirent = next(dirent);
    }


    Print("done searching (%u/%u)\n", seen, dir->size);

    // shorten directory inode
    Get_FS_Buffer(instance->fs_buf_cache, BLOCKNUM_FROM_INODENUM(dir_num), &buf);
    Modify_FS_Buffer(instance->fs_buf_cache, buf);
    dir = buf->data + OFFSET_IN_BLOCK(dir_num);
    unsigned int size_of_dirent = dirent->entry_length + 4;
    dir->size -= size_of_dirent; // reduce size with size of dirent
    print_inode(dir, dir_num);
    Release_FS_Buffer(instance->fs_buf_cache, buf);

    // delete dirent
    Get_FS_Buffer(instance->fs_buf_cache, dir->extents[0].start_block, &buf);
    Modify_FS_Buffer(instance->fs_buf_cache, buf);
    dirent = buf->data + (seen - (size_of_dirent));
    memset(dirent, '\0', size_of_dirent);
    Release_FS_Buffer(instance->fs_buf_cache, buf);

    // Delete inode
    Get_FS_Buffer(instance->fs_buf_cache,BLOCKNUM_FROM_INODENUM(num), &buf);
    Modify_FS_Buffer(instance->fs_buf_cache, buf);
    memset(buf->data, '\0', sizeof(struct gfs3_inode));
    Release_FS_Buffer(instance->fs_buf_cache, buf);


    return 0;




    TODO_P(PROJECT_GFS3, "GeekOS filesystem delete operation");
    return EUNSUPPORTED;
}

/*
 * Get metadata (size, permissions, etc.) of file named by given path.
 */
static int GFS3_Stat(struct Mount_Point *mountPoint, const char *path,
                     struct VFS_File_Stat *stat) {


    struct GFS3_Instance *instance = mountPoint->fsData;
    struct gfs3_inode *inode = 0;
    // Print("GFS3_Stat: path = %s\n", path);
    // struct gfs3_inode *n15 = get_inode(instance->fs_buf_cache, 15);
    // print_inode(n15, 15);


    gfs3_inodenum  num = lookup(instance,path,&inode);
    if (!inode || num == 0 ){
        return ENOTFOUND;
    }

    stat->size = inode->size;
    stat->isSetuid = 0;
    stat->isDirectory = (unsigned int)is_dir(inode);

    return 0;






    TODO_P(PROJECT_GFS3, "GeekOS filesystem stat operation");
    return EUNSUPPORTED;
}

/*
 * Synchronize the filesystem data with the disk
 * (i.e., flush out all buffered filesystem data).
 */
static int GFS3_Sync(struct Mount_Point *mountPoint) {
    TODO_P(PROJECT_GFS3, "GeekOS filesystem sync operation");
    return EUNSUPPORTED;
}

static int GFS3_Disk_Properties(struct Mount_Point *mountPoint,
                                unsigned int *block_size,
                                unsigned int *blocks_in_disk) {
    TODO_P(PROJECT_GFS3,
           "GeekOS filesystem infomation operation; set variables.");
    return EUNSUPPORTED;
}

/*static*/ struct Mount_Point_Ops s_gfs3MountPointOps = {
    &GFS3_Open,
    &GFS3_Create_Directory,
    &GFS3_Open_Directory,
    &GFS3_Stat,
    &GFS3_Sync,
    &GFS3_Delete,
    0,                          /* Rename  */
    0,                          /* Link  */
    0,                          /* SymLink  */
    0,                          /* setuid  */
    0,                          /* acl  */
    &GFS3_Disk_Properties,
};

static int GFS3_Format(struct Block_Device *blockDev
                       __attribute__ ((unused))) {
    TODO_P(PROJECT_GFS3,
           "DO NOT IMPLEMENT: There is no format operation for GFS3");
    return EUNSUPPORTED;
}

static int GFS3_Mount(struct Mount_Point *mountPoint) {

    if(mountPoint->fsData != NULL){
        Print("already mounted\n");
        return 0;
    }

    struct GFS3_Instance *instance = 0;


    // Allocate memory for instance.
    instance = (struct GFS3_Instance *)Malloc(sizeof(struct GFS3_Instance));
    if (instance == 0){ return ENOMEM; }
    memset(instance, '\0', sizeof(struct GFS3_Instance));

    // create fs-buffer
    instance->fs_buf_cache = Create_FS_Buffer_Cache(mountPoint->dev,GFS3_BLOCK_SIZE);


    // read superblock
    struct FS_Buffer *buf;
    int n = Get_FS_Buffer(instance->fs_buf_cache, GFS3_SUPERBLOCK, &buf);
    if (n != 0 ){
        Print("failed to get FS_Buffer\n");
        return EUNSPECIFIED;
    }
    instance->superblock = (struct gfs3_superblock *)(buf->data + PFAT_BOOT_RECORD_OFFSET);


    // check magic number
    if (instance->superblock->gfs3_magic != GFS3_MAGIC){
        Print("\tfound magic number 0x%x, but want 0x%x\n", instance->superblock->gfs3_magic, GFS3_MAGIC);
        return EINVALIDFS;
    }

    // check version number
    if (instance->superblock->gfs3_version != GFS3_VERSION){
        Print("\tfound version number 0x%x, but want 0x%x\n", instance->superblock->gfs3_version, GFS3_VERSION);
        return EINVALIDFS;
    }
    //print_superblock(instance->superblock);


    // read root node
    ulong_t root_block_num = instance->superblock->block_with_inode_zero;
    instance->block_with_root = root_block_num;


    n = Get_FS_Buffer(instance->fs_buf_cache, root_block_num, &buf);
    if (n != 0) { Print("\n failed to read block\n"); }
    instance->root_dir_inode = (struct gfs3_inode *)(buf->data + GFS3_INODE_SIZE); // inode 1 is root


    KASSERT(is_dir(instance->root_dir_inode));



    // save instance in mount point
    mountPoint->fsData = (void *)instance;
    mountPoint->ops = &s_gfs3MountPointOps;

    // create bitmap in inode 2
    instance->bitmap = (char *)Malloc(GFS3_BLOCK_SIZE); // must apparently allocate before...
    instance->bitmap = Create_Bit_Set(GFS3_BITMAP_SIZE);
    //Set_Bit(instance->bitmap, 0);
    //Set_Bit(instance->bitmap, 1);
    //Set_Bit(instance->bitmap, 2);
    // Print("BEFORE: first 10 blocks: %d\n", Find_First_N_Free(instance->bitmap, 10, GFS3_BITMAP_SIZE));

    Release_FS_Buffer(instance->fs_buf_cache, buf);


    // read bitmap stored in inode 2
    struct gfs3_inode *inode2 = get_inode(instance->fs_buf_cache, 2);

    struct FS_Buffer *bitmapBuf;
    int rc = Get_FS_Buffer(instance->fs_buf_cache,inode2->extents->start_block, &bitmapBuf);
    if (rc != 0){
        Print("\trc = %d\n",rc);
    }

    memcpy(instance->bitmap, bitmapBuf->data, GFS3_BLOCK_SIZE);
   //  Print("AFTER: first 10 blocks: %d\n", Find_First_N_Free(instance->bitmap,10, GFS3_BITMAP_SIZE));
    Release_FS_Buffer(instance->fs_buf_cache, bitmapBuf);
    // Print("done with mnt...\n");


    // TODO_P(PROJECT_GFS3, "GeekOS filesystem mount operation");
    return 0;
}


static struct Filesystem_Ops s_gfs3FilesystemOps = {
    &GFS3_Format,
    &GFS3_Mount,
};

/* ----------------------------------------------------------------------
 * Public functions
 * ---------------------------------------------------------------------- */

void Init_GFS3(void) {
    Register_Filesystem("gfs3", &s_gfs3FilesystemOps);
}
