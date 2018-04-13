/*
 * Paging-based user mode implementation
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
 *
 * $Revision: 1.51 $
 */

#include <geekos/int.h>
#include <geekos/mem.h>
#include <geekos/paging.h>
#include <geekos/malloc.h>
#include <geekos/string.h>
#include <geekos/argblock.h>
#include <geekos/kthread.h>
#include <geekos/range.h>
#include <geekos/vfs.h>
#include <geekos/user.h>
#include <geekos/projects.h>
#include <geekos/smp.h>
#include <geekos/synch.h>
#include <geekos/errno.h>
#include <geekos/gdt.h>
#include <geekos/paging.h>




extern Spin_Lock_t kthreadLock;

int userDebug = 0;
#define Debug(args...) if (userDebug) Print("uservm: " args)

/* ----------------------------------------------------------------------
 * Private functions
 * ---------------------------------------------------------------------- */

// TODO: Add private functions
extern void Load_LDTR(ushort_t LDTselector);


/* ----------------------------------------------------------------------
 * Public functions
 * ---------------------------------------------------------------------- */

/*
 * Destroy a User_Context object, including all memory
 * and other resources allocated within it.
 */
void Destroy_User_Context(struct User_Context *context) {
    /*
     * Hints:
     * - Free all pages, page tables, and page directory for
     *   the process (interrupts must be disabled while you do this,
     *   otherwise those pages could be stolen by other processes)
     * - Free semaphores, files, and other resources used
     *   by the process
     */
    //Print("Destroy_User_Context > start\n");
    // TODO_P(PROJECT_VIRTUAL_MEMORY_A, "Destroy User_Context data structure after process exits");
    int i, j;
    pde_t* page_dir = context->pageDir;
    pte_t* page_table;

    for(i = PAGE_DIRECTORY_INDEX(LIN_USER_BASE_ADDR); i < NUM_PAGE_DIR_ENTRIES; ++i) {
        if(page_dir[i].pageTableBaseAddr == '\0'){
            continue;
        }
        page_table = (pte_t *)(page_dir[i].pageTableBaseAddr<<12);
        for(j=0; j < NUM_PAGE_TABLE_ENTRIES; ++j) {
            Disable_Interrupts();
            if(page_table[j].present == 0 && page_table[j].kernelInfo == KINFO_PAGE_ON_DISK) {
                // on disk
                Free_Space_On_Paging_File(page_table[j].pageBaseAddr);
            } else if(page_table[j].present == 1) {
                void *addr = (void *)(page_table[j].pageBaseAddr<<12);
                if ((uint_t )addr == 0xFEE00000 || (uint_t)addr == 0xFEC00000){
                    // Don't free apic or apic-io
                    Enable_Interrupts();
                    continue;
                }
                //Print("page at 0x%x\n", (uint_t)addr);
                Free_Page(addr);
            }
            Enable_Interrupts();
        }
        Free_Page(page_table);
    }
    Free_Page(page_dir);

    Free_Segment_Descriptor(context->ldtDescriptor);
    Free(context);
    //Print("Destroy_User_Context > end\n");
}

/*
 * Load a user executable into memory by creating a User_Context
 * data structure.
 * Params:
 * exeFileData - a buffer containing the executable to load
 * exeFileLength - number of bytes in exeFileData
 * exeFormat - parsed ELF segment information describing how to
 *   load the executable's text and data segments, and the
 *   code entry point address
 * command - string containing the complete command to be executed:
 *   this should be used to create the argument block for the
 *   process
 * pUserContext - reference to the pointer where the User_Context
 *   should be stored
 *
 * Returns:
 *   0 if successful, or an error code (< 0) if unsuccessful
 */
int Load_User_Program(char *exeFileData, ulong_t exeFileLength,
                      struct Exe_Format *exeFormat, const char *command,
                      struct User_Context **pUserContext) {
    /*
     * Hints:
     * - This will be similar to the same function in userseg.c
     * - Determine space requirements for code, data, argument block,
     *   and stack
     * - Allocate pages for above, map them into user address
     *   space (allocating page directory and page tables as needed)
     * - Fill in initial stack pointer, argument block address,
     *   and code entry point fields in User_Context
     */

    //Print("Load_User_Program > start\n");
    // Determine space requirements argument block
    unsigned numArgs;
    ulong_t argBlockSize;
    Get_Argument_Block_Size(command, &numArgs, &argBlockSize);
    //Print("\t argBlockSize = %d\n", (int)argBlockSize);

    // Find maximum virtual address
    int i;
    ulong_t max_value = 0;
    for (i = 0; i < exeFormat->numSegments; ++i) {
        struct Exe_Segment *segment = &exeFormat->segmentList[i];
        ulong_t top_value = segment->startAddress + segment->sizeInMemory;

        if (top_value > max_value)
            max_value = top_value;
    }

    // Setup some memory space for the program
    unsigned long virtSize = Round_Up_To_Page(max_value);
    unsigned long lin_stack_ptr = PAGE_ADDR(LIN_END_OF_VM) - 0x1000;
    // Print("\tvirtSize = 0x%x\n", (unsigned int)virtSize);
    // Print("\tlin_stack_ptr = 0x%x\n", (unsigned int)lin_stack_ptr);


    // Copy all of the mappings from the kernel mode page directory
    pde_t* base_page_directory = 0;
    pde_t* page_dir_entry = 0;
    pte_t* base_page_table = 0;
    pte_t* pte = 0;
    base_page_directory = (pde_t*)Alloc_Page();
    memset(base_page_directory,'\0',PAGE_SIZE);            // Null char means that there is no entry
    memcpy(base_page_directory, Get_PDBR(), PAGE_SIZE/2);  // very important

    // Alloc user page dir entry(start from 0x80000000)
    // Print("\texeFormat->numSegments = %d \n", exeFormat->numSegments);
    for(i=0; i < exeFormat->numSegments; ++i){
        struct Exe_Segment *segment = &exeFormat->segmentList[i];
        ulong_t startAddress = LIN_USER_BASE_ADDR + segment->startAddress;

        // Print("\tbase_page_dir = 0x%x\n", (unsigned int)base_page_directory);
        page_dir_entry = &base_page_directory[PAGE_DIRECTORY_INDEX(startAddress)];

        // Alloc page table
        int j;
        int required_page_dir_entries = (int)PAGE_DIRECTORY_INDEX(segment->lengthInFile) +1;
        // Print("\t%d directory pages needed for segment #%d\n", required_page_dir_entries, i);
        for(j=0; j < required_page_dir_entries; j++) {
            // Page table is not exist
            if(page_dir_entry[j].pageTableBaseAddr == '\0') {
                // Print("\tpage_dir[%d] does not exist --> allocating it...\n", j);
                base_page_table = (pte_t*)Alloc_Page();
                memset(base_page_table,'\0',PAGE_SIZE);
            } else {
                base_page_table = (pte_t *)(page_dir_entry[j].pageTableBaseAddr<<12);
            }

            // Set up page directory entry.
            page_dir_entry[j].pageTableBaseAddr = (uint_t)PAGE_ALIGNED_ADDR(base_page_table);
            page_dir_entry[j].present = 1;
            page_dir_entry[j].flags = VM_USER | VM_WRITE;
            pte = &base_page_table[PAGE_TABLE_INDEX(startAddress)];

            // Alloc page table entry and map to physical address
            ulong_t lin_addr = 0;
            void* phy_addr = 0;
            int required_page_table_entries = (int)PAGE_TABLE_INDEX(segment->lengthInFile) + 1;
            // Print("\t\t%d table pages needed for segment #%d in page_dir[%d]\n", required_page_table_entries, i, j);
            int k;
            for(k=0; k < required_page_table_entries; k++) {
                lin_addr = PAGE_ADDR(startAddress + PAGE_ADDR_BY_IDX(j, k)); // lin_addr?
                phy_addr = Alloc_Pageable_Page(&pte[k], lin_addr);
                //Print("\t\t\tlin_addr = 0x%x\tphy_addr = 0x%x\n", (uint_t )lin_addr, (uint_t )phy_addr );
                pte[k].pageBaseAddr = PAGE_ALIGNED_ADDR(phy_addr);

                // Copy data from segment to pages
                const void *src =  exeFileData + PAGE_ADDR(segment->offsetInFile + PAGE_ADDR_BY_IDX(j, k));
                //Print("\t\t\tcopy src(0x%x) --> dst(0x%x)\n", (uint_t)src, (uint_t)phy_addr);
                memcpy(phy_addr, src, PAGE_SIZE);
                pte[k].present = 1;
                pte[k].flags = VM_USER | VM_WRITE;
            }

        }
    }


    // Alloc stack
    int pd_index = PAGE_DIRECTORY_INDEX(lin_stack_ptr);
    // Print("\t===Alloc stack===\n");
    // Print("\t\tpage directory index of lin_stack_ptr(0x%x) = %d\n",(uint_t)lin_stack_ptr, pd_index);

    page_dir_entry = &base_page_directory[pd_index];
    pte = (pte_t*)Alloc_Page();
    memset(pte,'\0',PAGE_SIZE);
    page_dir_entry->pageTableBaseAddr = (uint_t)PAGE_ALIGNED_ADDR(pte);
    page_dir_entry->present = 1;
    page_dir_entry->flags = VM_USER | VM_WRITE;


    // 4MB stack...
    int k;
    ulong_t lin_addr = 0;
    void* phy_addr = 0;
    int start_index = NUM_PAGE_TABLE_ENTRIES-PAGE_TABLE_INDEX(DEFAULT_USER_STACK_SIZE + argBlockSize)-1;
    // Print("\t\tstart_index = %d\n", start_index);
    for(k = start_index; k < NUM_PAGE_TABLE_ENTRIES; k++){
        lin_addr = PAGE_ADDR_BY_IDX(pd_index, k);
        phy_addr = Alloc_Pageable_Page(&pte[k], lin_addr);
        pte[k].pageBaseAddr = PAGE_ALIGNED_ADDR(phy_addr);
        pte[k].present = 1;
        pte[k].flags = VM_USER | VM_WRITE;
        // Print("\t\talloc pageable page at phy_addr = 0x%x\t lin_addr =0x%x\n ", (uint_t)phy_addr, (uint_t)lin_addr);
    }

    //0xFEE00000, VM_WRITE
    //0xFEC00000, VM_WRITE

    // Identity map APIC and APIC-IO
    Identity_Map_Page(base_page_directory,0xFEE00000, VM_WRITE);
    Identity_Map_Page(base_page_directory,0xFEC00000, VM_WRITE);



    // Print("\t=== Argument block===\n");
    unsigned long log_stack_ptr = lin_stack_ptr - LIN_USER_BASE_ADDR;
    Format_Argument_Block(phy_addr, numArgs, log_stack_ptr, command);
    // Print("\t\tlog_stack_ptr = 0x%x\n", (uint_t)log_stack_ptr);


    *pUserContext = (struct User_Context*)Malloc(sizeof(struct User_Context));
    (*pUserContext)->size = virtSize;
    (*pUserContext)->entryAddr = exeFormat->entryAddr;
    (*pUserContext)->stackPointerAddr = log_stack_ptr;
    (*pUserContext)->argBlockAddr = log_stack_ptr;          // just start from stack pointer
    (*pUserContext)->refCount = 0;                          // important
    (*pUserContext)->pageDir = base_page_directory;         // important

    // setup LDT
    // alloc LDT seg desc in GDT
    struct Segment_Descriptor* desc;
    desc = Allocate_Segment_Descriptor();
    Init_LDT_Descriptor(desc,
            ((*pUserContext)->ldt), // base address
            NUM_USER_LDT_ENTRIES    // num pages
    );
    unsigned short LDTSelector;
    LDTSelector = Selector(KERNEL_PRIVILEGE, true, Get_Descriptor_Index(desc));
    (*pUserContext)->ldtDescriptor = desc;
    (*pUserContext)->ldtSelector = LDTSelector;

    desc = &((*pUserContext)->ldt)[0];
    Init_Code_Segment_Descriptor(
            desc,
            (unsigned long)LIN_USER_BASE_ADDR, // base address
            (LIN_USER_BASE_ADDR/PAGE_SIZE),  // need to modify
            USER_PRIVILEGE		   // privilege level (0 == kernel)
    );
    unsigned short codeSelector = Selector(USER_PRIVILEGE, false, 0);
    (*pUserContext)->csSelector = codeSelector;

    desc = &((*pUserContext)->ldt)[1];
    Init_Data_Segment_Descriptor(
            desc,
            (unsigned long)LIN_USER_BASE_ADDR, // base address
            (LIN_USER_BASE_ADDR/PAGE_SIZE),  // num pages
            USER_PRIVILEGE		   // privilege level (0 == kernel)
    );
    unsigned short dataSelector = Selector(USER_PRIVILEGE, false, 1);
    (*pUserContext)->dsSelector = dataSelector;



    // TODO_P(PROJECT_VIRTUAL_MEMORY_A, "Load user program into address space");
    // Print("Load_User_Program > end\n");

    return 0;
}

/*
 * Copy data from user buffer into kernel buffer.
 * Returns true if successful, false otherwise.
 */
bool Copy_From_User(void *destInKernel, ulong_t srcInUser,
                    ulong_t numBytes) {
    /*
     * Hints:
     * - Make sure that user page is part of a valid region
     *   of memory
     * - Remember that you need to add 0x80000000 to user addresses
     *   to convert them to kernel addresses, because of how the
     *   user code and data segments are defined
     * - User pages may need to be paged in from disk before being accessed.
     * - Before you touch (read or write) any data in a user
     *   page, **disable the PAGE_PAGEABLE bit**.
     *
     * Be very careful with race conditions in reading a page from disk.
     * Kernel code must always assume that if the struct Page for
     * a page of memory has the PAGE_PAGEABLE bit set,
     * IT CAN BE STOLEN AT ANY TIME.  The only exception is if
     * interrupts are disabled; because no other process can run,
     * the page is guaranteed not to be stolen.
     */


    memcpy(destInKernel, (void*)(LIN_USER_BASE_ADDR + srcInUser), numBytes); // because kernel mode
    //Print("Copy_From_User\n");
    return true;
    // TODO_P(PROJECT_VIRTUAL_MEMORY_A, "Copy user data to kernel buffer");
}

/*
 * Copy data from kernel buffer into user buffer.
 * Returns true if successful, false otherwise.
 */
bool Copy_To_User(ulong_t destInUser, const void *srcInKernel,
                  ulong_t numBytes) {
    /*
     * Hints:
     * - Same as for Copy_From_User()
     * - Also, make sure the memory is mapped into the user
     *   address space with write permission enabled
     */
    //Print("Copy_To_User\n");
    memcpy((void*)(LIN_USER_BASE_ADDR + destInUser), srcInKernel, numBytes);
    return true;
    //TODO_P(PROJECT_VIRTUAL_MEMORY_A, "Copy kernel data to user buffer");
}


/*
 * Switch to user address space.
 */
void Switch_To_Address_Space(struct User_Context *userContext) {
    /*
     * - If you are still using an LDT to define your user code and data
     *   segments, switch to the process's LDT
     * - 
     */
    //Print("Switch_To_Address_Space > start\n");
    Load_LDTR(userContext->ldtSelector);
    Set_PDBR(userContext->pageDir);
    //Print("Switch_To_Address_Space > end\n");
    //TODO_P(PROJECT_VIRTUAL_MEMORY_A, "Switch_To_Address_Space() using paging");
}
