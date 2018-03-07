/*
 * Copyright (c) 2001,2003,2004 David H. Hovemeyer <daveho@cs.umd.edu>
 * Copyright (c) 2003,2013,2014 Jeffrey K. Hollingsworth <hollings@cs.umd.edu>
 *
 * All rights reserved.
 *
 * This code may not be resdistributed without the permission of the copyright holders.
 * Any student solutions using any of this code base constitute derviced work and may
 * not be redistributed in any form.  This includes (but is not limited to) posting on
 * public forums or web sites, providing copies to (past, present, or future) students
 * enrolled in similar operating systems courses the University of Maryland's CMSC412 course.
 */
#include <geekos/pipe.h>
#include <geekos/malloc.h>
#include <geekos/string.h>
#include <geekos/errno.h>
#include <geekos/projects.h>
#include <geekos/int.h>
#include <geekos/signal.h>

#define PIPE_MAX_SIZE (32*1024) // 32K

const struct File_Ops Pipe_Read_Ops  = { NULL, Pipe_Read, NULL, NULL, Pipe_Close, NULL };
const struct File_Ops Pipe_Write_Ops = { NULL, NULL, Pipe_Write, NULL, Pipe_Close, NULL };

int Pipe_Create(struct File **read_file, struct File **write_file) {
    struct Pipe *pipe = (struct Pipe *)Malloc(sizeof(struct Pipe));
    if (pipe == NULL) { 
        return ENOMEM; 
    }

    // Create mutex.
    pipe->mu = (struct Mutex *)Malloc(sizeof(struct Mutex));
    Mutex_Init(pipe->mu);

    // Initialize pipe struct as a 1-to-1 pipe.
    pipe->readers = 1;
    pipe->writers = 1;
    pipe->read_idx = 0;
    pipe->write_idx = 0;

    // Allocate space for buffer.
    pipe->buffer = Malloc(PIPE_MAX_SIZE);
    if (pipe->buffer == NULL) {
        return ENOMEM;
    }
    pipe->buffer_bytes = 0;

    // Allocate files.
    *read_file = Allocate_File(&Pipe_Read_Ops, 0 ,0, pipe, 0, NULL);
    *write_file = Allocate_File(&Pipe_Write_Ops, 0, 0, pipe, 0, NULL);
    if (*read_file == NULL || *write_file == NULL) {
        return ENOMEM;
    }

    SetRefCount((*read_file), 1);
    SetRefCount((*write_file), 1);
    //(*write_file)->refcount = 1;

    return 0;

}

int Pipe_Read(struct File *f, void *buf, ulong_t numBytes) {
    struct Pipe *pipe = (struct Pipe *)f->fsData;

    Mutex_Lock(pipe->mu);

    bool writers_present = pipe->writers > 0;
    bool no_data = pipe->buffer_bytes == 0;
    if (writers_present && no_data) {
        Mutex_Unlock(pipe->mu);
        return EWOULDBLOCK;
    }
    
    if (no_data) {
        Mutex_Unlock(pipe->mu);
        return 0;
    }

    // read from pipe buffer.
    char *dst = (char *)buf;
    char *src = (char *)(pipe->buffer);
    ulong_t i;
    ulong_t read_pos = pipe->read_idx % PIPE_MAX_SIZE;
    for (i = 0; i < numBytes && i < pipe->buffer_bytes; i++) {
        dst[i] = src[read_pos];
        read_pos = (read_pos + 1)% PIPE_MAX_SIZE;
    }
    pipe->buffer_bytes -= i;
    pipe->read_idx = read_pos;

    Mutex_Unlock(pipe->mu);
    return (int)i;
}

int Pipe_Write(struct File *f, void *buf, ulong_t numBytes) {
    struct Pipe *pipe = (struct Pipe *)f->fsData;
    Mutex_Lock(pipe->mu);

    // No readers.
    if (pipe->readers == 0) {
        Mutex_Unlock(pipe->mu);
        return EPIPE;
    }

    // write to pipe.
    char *dst = (char *)(pipe->buffer);
    char *src = (char *)buf;
    ulong_t i;
    ulong_t write_pos = pipe->write_idx % PIPE_MAX_SIZE;
    ulong_t free_space_in_buffer = PIPE_MAX_SIZE - pipe->buffer_bytes;
    for (i = 0; i < numBytes && i < free_space_in_buffer; i++) {
        dst[write_pos] = src[i];
        write_pos = (write_pos + 1) % PIPE_MAX_SIZE;
    }
    pipe->buffer_bytes += i;
    pipe->write_idx = write_pos;

    Mutex_Unlock(pipe->mu);
    return (int)i;
}

int Pipe_Close(struct File *f) {
    struct Pipe *pipe = (struct Pipe *)f->fsData;
    Mutex_Lock(pipe->mu);

    if (f->refcount == 0){
    // determine what end we're on
        bool is_reader = f->ops->Read != NULL;
        if (is_reader) {
            pipe->readers--;
        }
        bool is_writer = f->ops->Write != NULL;
        if (is_writer) {
            pipe->writers--;
        }
    }

    // close pipe if no readers
    if (pipe->readers == 0 && pipe->writers == 0) {
        Mutex_Unlock(pipe->mu);
        Free(pipe->mu);
        Free(pipe->buffer);
        Free(pipe);
        return 0;
    }

    Mutex_Unlock(pipe->mu);
    return 0;
}
