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
#include <geekos/vfs.h>

extern const struct File_Ops Pipe_Read_Ops;
extern const struct File_Ops Pipe_Write_Ops;

/*
 * Pipe object contains:
 * readers - the number of readers
 * writers - the number of writers
 * buffer - the pointer to data buffer.
 */
struct Pipe {
    int readers;
    int writers;
    ulong_t read_idx;
    ulong_t write_idx;
    void *buffer;
    ulong_t buffer_bytes;
};

int Pipe_Create(struct File **read_file, struct File **write_file);
int Pipe_Read(struct File *f, void *buf, ulong_t numBytes);
int Pipe_Write(struct File *f, void *buf, ulong_t numBytes);
int Pipe_Close(struct File *f);
