/* This file is a placeholder for storing per-cpu variables in a segment that is not 
   saved and restored per thread, but rather left alone although different on a per-cpu
   basis. */

#include <geekos/kthread.h>


#define MAX_CPU_2 8


struct CPU_Local_Storage{
    struct Kernel_Thread *kthread;
    int cpu;
};



void Init_PerCPU(int cpu);
int PerCPU_Get_CPU(void);
struct Kernel_Thread *PerCPU_Get_Current(void);
