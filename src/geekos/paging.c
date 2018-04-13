/*
 * Paging (virtual memory) support
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
 * $Revision: 1.56 $
 * 
 */

#include <geekos/string.h>
#include <geekos/int.h>
#include <geekos/idt.h>
#include <geekos/kthread.h>
#include <geekos/kassert.h>
#include <geekos/screen.h>
#include <geekos/mem.h>
#include <geekos/malloc.h>
#include <geekos/gdt.h>
#include <geekos/segment.h>
#include <geekos/user.h>
#include <geekos/vfs.h>
#include <geekos/crc32.h>
#include <geekos/paging.h>
#include <geekos/errno.h>
#include <geekos/projects.h>
#include <geekos/smp.h>

#include <libc/mmap.h>

/* ----------------------------------------------------------------------
 * Public data
 * ---------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
 * Private functions/data
 * ---------------------------------------------------------------------- */

#define SECTORS_PER_PAGE (PAGE_SIZE / SECTOR_SIZE)

/*
 * flag to indicate if debugging paging code
 */
int debugFaults = 0;
#define Debug(args...) if (debugFaults) Print(args)


static pde_t *kernel_page_dir;
/* const because we do not expect any caller to need to
   modify the kernel page directory */
const pde_t *Kernel_Page_Dir(void) {
    return kernel_page_dir;
    // TODO_P(PROJECT_VIRTUAL_MEMORY_A, "return kernel page directory");
    // return NULL;
}



/*
 * Print diagnostic information for a page fault.
 */
static void Print_Fault_Info(uint_t address, faultcode_t faultCode) {
    extern uint_t g_freePageCount;
    struct Kernel_Thread *current = get_current_thread(0);      /* informational, could be incorrect with low probability */

    if(current) {
        Print("Pid %d: (%p/%s)", current->pid, current,
              current->threadName);
    }
    Print("\n Page Fault received, at address %p (%d pages free)\n",
          (void *)address, g_freePageCount);
    if(faultCode.protectionViolation)
        Print("   Protection Violation, ");
    else
        Print("   Non-present page, ");
    if(faultCode.writeFault)
        Print("Write Fault, ");
    else
        Print("Read Fault, ");
    if(faultCode.userModeFault)
        Print("in User Mode\n");
    else
        Print("in Supervisor Mode\n");
}

union type_pun_workaround {
    faultcode_t faultCode;
    ulong_t errorCode;
};

/*
 * Handler for page faults.
 * You should call the Install_Interrupt_Handler() function to
 * register this function as the handler for interrupt 14.
 */
/*static*/ void Page_Fault_Handler(struct Interrupt_State *state) {
    ulong_t address;
    union type_pun_workaround tpw;
    faultcode_t faultCode;

    KASSERT(!Interrupts_Enabled());

    /* Get the address that caused the page fault */
    address = Get_Page_Fault_Address();
    Debug("Page fault @%lx\n", address);


    if(address < 0xfec01000 && address > 0xf0000000) {
        Print("page fault address in APIC/IOAPIC range\n");
        goto error;
    }

    /* Get the fault code */
    tpw.errorCode = state->errorCode;
    faultCode = tpw.faultCode;
    // faultCode = *((faultcode_t *) &(state->errorCode));

    /* rest of your handling code here */
    //TODO_P(PROJECT_VIRTUAL_MEMORY_B, "handle page faults");
    Print("Page fault! eip = 0x%x\n", (uint_t )state->eip);
    if(faultCode.protectionViolation == 0) // Non-present page
    {
        pde_t* pde;
        pte_t* pte;
        void* paddr;
        uint_t kernelInfo;
        int j, k = 0;

        j = PAGE_DIRECTORY_INDEX(address);
        pde = &(Get_PDBR()[j]);
        if(pde->pageTableBaseAddr == '\0') {
            pte = (pte_t*)Alloc_Page();
            memset(pte,'\0',PAGE_SIZE);
            pde->pageTableBaseAddr = (uint_t)PAGE_ALIGNED_ADDR(pte);
            pde->present = 1;
            pde->flags = VM_USER | VM_WRITE;
        } else {
            pte = (pte_t *)(pde->pageTableBaseAddr<<12);
        }

        k = PAGE_TABLE_INDEX(address);
        kernelInfo = pte[k].kernelInfo;
        paddr = Alloc_Pageable_Page(&pte[k], PAGE_ADDR(address));
        if(paddr == 0){
//            if(Get_Current()->pid != sh_pid)
//                Exit(-1);
        }

        if(kernelInfo == KINFO_PAGE_ON_DISK) // case 2
        {
            Print ("KINFO_PAGE_ON_DISK\n");
            Enable_Interrupts();
            Read_From_Paging_File(paddr, address, pte[k].pageBaseAddr);
            Disable_Interrupts();
            Free_Space_On_Paging_File(pte[k].pageBaseAddr);
        }

        pte[k].present = 1;
        pte[k].flags = VM_USER | VM_WRITE;
        pte[k].pageBaseAddr = PAGE_ALIGNED_ADDR(paddr);

        return;
    }




    TODO_P(PROJECT_MMAP, "handle mmap'd page faults");


  error:
    Print("Unexpected Page Fault received %p\n", (void *) address);
    Print_Fault_Info(address, faultCode);
    Dump_Interrupt_State(state);
    /* user faults just kill the process; not user mode faults should halt the kernel. */
    KASSERT0(faultCode.userModeFault,
             "unhandled kernel-mode page fault.");

    /* For now, just kill the thread/process. */
    Enable_Interrupts();
    Exit(-1);
}

void Identity_Map_Page(pde_t *base_page_dir, unsigned int address, int flags) {
    // Print("Identity mapping 0x%x\n", address);

    int pd_index = PAGE_DIRECTORY_INDEX(address);
    // Print("Identity mapping  pd_index = %d\n", pd_index);


    pte_t *page_table = 0;
    pde_t *currentPageDirEntry = base_page_dir + pd_index;

    // Allocate page dir entry if not present.
    if (!currentPageDirEntry->present){
        // Print("allocation page directory entry aka a page table...\n");
        page_table = (pte_t*)Alloc_Page();
        memset(page_table,'\0',PAGE_SIZE);
        currentPageDirEntry->present = 1;              // flag this page as present
        currentPageDirEntry->flags = VM_WRITE | VM_USER;         // set permissions
        currentPageDirEntry->pageTableBaseAddr = PAGE_ALIGNED_ADDR(page_table);


    } else {
        // cool we already have a page dir entry
        page_table = (pte_t *)(currentPageDirEntry->pageTableBaseAddr << 12);
    }

    int pt_index = PAGE_TABLE_INDEX(address);
    // Print("Identity mapping  pt_index = %d\n", pt_index);
    if (!page_table[pt_index].present){
        page_table[pt_index].present = 1;
        page_table[pt_index].flags = flags;
        page_table[pt_index].pageBaseAddr = PAGE_ALIGNED_ADDR(address);
        page_table[pt_index].flags = VM_USER | VM_WRITE;
    }

}

void Identity_Map_Page2(unsigned int address, int flags) {
    // Print("Identity mapping 0x%x\n", address);

    int pd_index = PAGE_DIRECTORY_INDEX(address);
    // Print("Identity mapping  pd_index = %d\n", pd_index);


    pte_t *page_table = 0;
    pde_t *currentPageDirEntry = kernel_page_dir + pd_index;

    // Allocate page dir entry if not present.
    if (!currentPageDirEntry->present){
        page_table = (pte_t*)Alloc_Page();
        memset(page_table,'\0',PAGE_SIZE);
        currentPageDirEntry->present = 1;              // flag this page as present
        currentPageDirEntry->flags = VM_WRITE | VM_USER;         // set permissions
        currentPageDirEntry->pageTableBaseAddr = PAGE_ALIGNED_ADDR(page_table);


    } else {
        // cool we already have a page dir entry
        page_table = (pte_t *)(currentPageDirEntry->pageTableBaseAddr << 12);
    }

    int pt_index = PAGE_TABLE_INDEX(address);
    // Print("Identity mapping  pt_index = %d\n", pt_index);
    if (!page_table[pt_index].present){
        //Print("allocation page table entry aka a page\n");
        page_table[pt_index].present = 1;
        page_table[pt_index].flags = flags;
        page_table[pt_index].pageBaseAddr = PAGE_ALIGNED_ADDR(address);
        page_table[pt_index].flags = VM_USER | VM_WRITE;
    }
}

/* ----------------------------------------------------------------------
 * Public functions
 * ---------------------------------------------------------------------- */


/*
 * Initialize virtual memory by building page tables
 * for the kernel and physical memory.
 */
void Init_VM(struct Boot_Info *bootInfo) {
    /*
     * Hints:
     * - Build kernel page directory and page tables
     * - Call Enable_Paging() with the kernel page directory
     * - Install an interrupt handler for interrupt 14,
     *   page fault
     * - Do not map a page at address 0; this will help trap
     *   null pointer references
     */

    int i, j = 0;
    pde_t* page_directory = 0;
    pte_t* page_table = 0;

    // Allocate page directory
    page_directory = (pde_t*)Alloc_Page();
    memset(page_directory,'\0',PAGE_SIZE); // set all zeroes



    int memSizeB = bootInfo->memSizeKB * 1024;
    KASSERT(PAGE_DIRECTORY_INDEX(memSizeB) <= NUM_PAGE_DIR_ENTRIES);
    // Print("need %d page directory entries\n",PAGE_DIRECTORY_INDEX(memSizeB));
    uint_t counter = 0;
    for (i=0; i <= PAGE_DIRECTORY_INDEX(memSizeB); i++) {
        page_table = (pte_t*)Alloc_Page();          // Alloc a page table
        memset(page_table,'\0',PAGE_SIZE);
        page_directory[i].present = 1;              // flag this page as present
        page_directory[i].flags = VM_WRITE | VM_USER;          // set permissions

        // Link page into page directory
        page_directory[i].pageTableBaseAddr = (uint_t)PAGE_ALIGNED_ADDR(page_table);


        // set up pages in page table
        for(j=0; j < NUM_PAGE_TABLE_ENTRIES; j++) {
            if (j == 0 && i == 0){
                // skip first page
                // Print("skipping page %d in table %d at addr %x\n", j, i, counter);
                counter += PAGE_SIZE;
                continue;
            }

            page_table[j].pageBaseAddr = PAGE_ALIGNED_ADDR(counter);
            counter += PAGE_SIZE;
            page_table[j].present = 1;
            page_table[j].flags = VM_USER | VM_WRITE;
        }
    }

    // Save kernel page directory.
    kernel_page_dir = page_directory;

    Identity_Map_Page2(0xFEE00000, VM_WRITE);
    Identity_Map_Page2(0xFEC00000, VM_WRITE);



    Enable_Paging(page_directory);
    Install_Interrupt_Handler(14, Page_Fault_Handler);
    Install_Interrupt_Handler(46, Page_Fault_Handler);
    // TODO_P(PROJECT_VIRTUAL_MEMORY_A, "Build initial kernel page directory and page tables");
}

void Init_Secondary_VM() {
    Enable_Paging(kernel_page_dir);
    //TODO_P(PROJECT_VIRTUAL_MEMORY_A, "enable paging on secondary cores");
}


static char* swapMap;
static uint_t totalPage;
static ulong_t startSector;
struct Block_Device* dev;


/**
 * Initialize paging file data structures.
 * All filesystems should be mounted before this function
 * is called, to ensure that the paging file is available.
 */
void Init_Paging(void) {

    struct Paging_Device *pagedev = Get_Paging_Device();
    dev = pagedev->dev;
    totalPage = (pagedev->numSectors)/SECTORS_PER_PAGE;
    startSector = pagedev->startSector;
    swapMap = (char*)Malloc(totalPage);
    memset(swapMap,0,totalPage);

    // Print("Init_Paging done\n");

    // TODO_P(PROJECT_VIRTUAL_MEMORY_B, "Initialize paging file data structures");
}

/* guards your structure for tracking free space on the paging file. */
static Spin_Lock_t s_free_space_spin_lock;

/**
 * Find a free bit of disk on the paging file for this page.
 * Interrupts must be disabled.
 * @return index of free page sized chunk of disk space in
 *   the paging file, or -1 if the paging file is full
 */
int Find_Space_On_Paging_File(void) {
    int retval = -1;
    int i;
    int iflag = Begin_Int_Atomic();
    Spin_Lock(&s_free_space_spin_lock);
    // TODO_P(PROJECT_VIRTUAL_MEMORY_B, "Find free page in paging file");

    for(i = 0; i < (int) totalPage; i++){
        if(swapMap[i] == 0){
            retval = i;
        }
    }

    // retval = EUNSUPPORTED;
    Spin_Unlock(&s_free_space_spin_lock);
    End_Int_Atomic(iflag);
    return retval;
    //return -1;
}

/**
 * Free a page-sized chunk of disk space in the paging file.
 * Interrupts must be disabled.
 * @param pagefileIndex index of the chunk of disk space
 */
void Free_Space_On_Paging_File(int pagefileIndex) {
    int iflag = Begin_Int_Atomic();
    Spin_Lock(&s_free_space_spin_lock);

    //TODO_P(PROJECT_VIRTUAL_MEMORY_B, "Free page in paging file");
    swapMap[pagefileIndex] = 0;


    Spin_Unlock(&s_free_space_spin_lock);
    End_Int_Atomic(iflag);
}

/**
 * Write the contents of given page to the indicated block
 * of space in the paging file.
 * @param paddr a pointer to the physical memory of the page
 * @param vaddr virtual address where page is mapped in user memory
 * @param pagefileIndex the index of the page sized chunk of space
 *   in the paging file
 */
void Write_To_Paging_File(void *paddr, ulong_t vaddr, int pagefileIndex) {
    struct Page *page = Get_Page((ulong_t) paddr);
    KASSERT(!(page->flags & PAGE_PAGEABLE));    /* Page must be pageable! */
    KASSERT(page->flags & PAGE_LOCKED); /* Page must be locked! */
    // TODO_P(PROJECT_VIRTUAL_MEMORY_B, "Write page data to paging file");

    int i;
    block_t* block = (block_t*)paddr;
    //struct Page *page = Get_Page((ulong_t) paddr);
    KASSERT(!(page->flags & PAGE_PAGEABLE)); /* Page must be locked! */
    for(i = 0; i < SECTORS_PER_PAGE; i++){
        Block_Write(dev, startSector+pagefileIndex*SECTORS_PER_PAGE+i, &block[i]);
    }
    swapMap[pagefileIndex] = 1;


}

/**
 * Read the contents of the indicated block
 * of space in the paging file into the given page.
 * @param paddr a pointer to the physical memory of the page
 * @param vaddr virtual address where page will be re-mapped in
 *   user memory
 * @param pagefileIndex the index of the page sized chunk of space
 *   in the paging file
 */
void Read_From_Paging_File(void *paddr, ulong_t vaddr, int pagefileIndex) {
    struct Page *page = Get_Page((ulong_t) paddr);
    KASSERT(!(page->flags & PAGE_PAGEABLE));    /* Page must be locked! */
    // TODO_P(PROJECT_VIRTUAL_MEMORY_B, "Read page data from paging file");
    int i;
    block_t* block = (block_t*)paddr;
    //struct Page *page = Get_Page((ulong_t) paddr);
    for(i = 0; i < SECTORS_PER_PAGE; i++){
        Block_Read(dev, startSector+pagefileIndex*SECTORS_PER_PAGE+i, &block[i]);
    }
}


void *Mmap_Impl(void *ptr, unsigned int length, int prot, int flags,
                int fd) {
    TODO_P(PROJECT_MMAP, "Mmap setup mapping");
    return NULL;
}

bool Is_Mmaped_Page(struct User_Context * context, ulong_t vaddr) {
    TODO_P(PROJECT_MMAP,
           "is this passed vaddr an mmap'd page in the passed user context");
    return false;
}

void Write_Out_Mmaped_Page(struct User_Context *context, ulong_t vaddr) {
    TODO_P(PROJECT_MMAP, "Mmap write back dirty mmap'd page");
}

int Munmap_Impl(ulong_t ptr) {
    TODO_P(PROJECT_MMAP, "unmapp the pages");
    return 0;
}
