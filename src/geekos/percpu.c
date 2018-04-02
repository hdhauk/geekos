
#include <geekos/percpu.h>
#include <limits.h>




//static struct CPU_Local_Storage CPU_Data_Table[MAX_CPU_2];
//static struct CPU_Local_Storage percpu[MAX_CPU_2];


void Init_PerCPU(int cpu){
    // CPU_Data_Table[cpu] = (struct CPU_Local_Storage){.cpu = cpu, .kthread = NULL };

    struct CPU_Local_Storage data = (struct CPU_Local_Storage){.kthread = NULL, .cpu = cpu};
    Print("Init for cpu %d\n", cpu);
    // Write cpu to gs:4
    asm volatile(
    "movl %0, %%gs:(4);\n\t"
    ::"r"(data.cpu)
    :
    );

    int ret = 69;
    asm volatile(
    "movl %%gs:4, %0;\n\t"
    :"=r"(ret)
    :
    :
    );
    Print("cpu%d ret = %d\n", cpu, ret);

}


int PerCPU_Get_CPU(void){
    int ret = -1;
    asm volatile(
    "movl %%gs:4, %0;\n\t"
    :"=r"(ret)
    :
    :
    );
    KASSERT(ret != -1);
    return ret;
}


struct Kernel_Thread *PerCPU_Get_Current(void){
    uint_t kthread = 69;
    asm volatile(
    "movl %%gs:0, %0;\n\t"
    :"=r"(kthread)
    :
    :
    );


    KASSERT(kthread > 0);
    KASSERT(kthread != 69);
    return (struct Kernel_Thread *)kthread;
}

