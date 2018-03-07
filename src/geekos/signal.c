/*************************************************************************/
/*
 * GeekOS master source distribution and/or project solution
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
/*************************************************************************/
/*
 * Signals
 * $Rev $
 * 
 * This is free software.  You are permitted to use,
 * redistribute, and modify it as specified in the file "COPYING".
 */

#include <geekos/kassert.h>
#include <geekos/defs.h>
#include <geekos/screen.h>
#include <geekos/int.h>
#include <geekos/mem.h>
#include <geekos/symbol.h>
#include <geekos/string.h>
#include <geekos/kthread.h>
#include <geekos/malloc.h>
#include <geekos/user.h>
#include <geekos/signal.h>
#include <geekos/projects.h>
#include <geekos/alarm.h>
#include <geekos/smp.h>

void Print_IS(struct Interrupt_State *esp);

void Signal_Ignore(int sig)
{
    //Print("Signal %d ignored\n",sig);
    if (!Interrupts_Enabled()){
        Print("Signal_Ignore > Enabling interrupts\n");
        Enable_Interrupts();
    }
}

void Signal_Default(int sig)
{
    Print("Terminated %d.\n",Get_Current()->pid);
    Enable_Interrupts();
    Exit(256 + sig);
}

void Send_Signal(struct Kernel_Thread *kthread, int sig)
{
    // Print("user_ctx->signal = %d\n", sig);
    kthread->userContext->signal = sig;
}

void Set_Handler(struct Kernel_Thread *kthread, int sig, signal_handler handler) {
    kthread->userContext->handlers[sig] = handler;
}

/* Called when signal handling is complete. */
void Complete_Handler(struct Kernel_Thread *kthread,
                      struct Interrupt_State *state) {
    /*
     * This routine should be called when the Sys ReturnSignal
     * call is invoked (when a signal handler has completed).
     * It needs to restore back on the top of the kernel stack
     * the snapshot of the interrupt state currently on the
     * top of the user stack.
     * */
    KASSERT(kthread);
    KASSERT(state);

    if (!Interrupts_Enabled()){
        Print("Complete_Handler > Enabling interrupts\n");
        Enable_Interrupts();
    }


    struct User_Interrupt_State *user_interrupt_state = (struct User_Interrupt_State *)state;
    uint_t usr_sp = user_interrupt_state->espUser;
    usr_sp += sizeof(int); // don't care about the signal...


    // Copy back Interrupt_State
    Copy_From_User(state, usr_sp, sizeof(struct Interrupt_State));
    usr_sp += sizeof(struct Interrupt_State);

    user_interrupt_state->espUser = usr_sp;

    Print("Complete_Handler > state->eax = %d \n", state->eax);
}

int Check_Pending_Signal(struct Kernel_Thread *kthread,
                         struct Interrupt_State *state) {
    /*
     * This is called by code in lowlevel.asm when a kernel
     * thread is about to be dispatched. It returns true
     * if the following THREE conditions hold:
     *      1.  A signal is pending for that process process.
     *      2.  The process is about to start executing in user space.
     *          This can be determined by checking the Interrupt State’s
     *          CS register: if it is not the kernel’s CS register
     *          (see include/geekos/defs.h), then the process is about
     *          to return to user space.
     *      3.  The process is not currently handling another signal
     *          (recall that signal handling is non-reentrant).
     * */
    KASSERT(kthread);
    KASSERT(state);


    if(!kthread->userContext)
    {
        return 0;
    }

    // Check if about to start executing in user space.
    if (state->cs == KERNEL_CS) {
        return 0;
    }

    int sig = kthread->userContext->signal;
    // No signal waiting.
    if (sig == 0)
    {
        return 0;
    }

    //Print("KERNEL > Check_Pending_Signal: found pending signal %d\n",sig);
    KASSERT(IS_SIGNUM(sig));


    return sig;

}

#if 0
void Print_IS(struct Interrupt_State *esp) {
    void **p;
    Print("esp=%x:\n", (unsigned int)esp);
    Print("  gs=%x\n", (unsigned int)esp->gs);
    Print("  fs=%x\n", (unsigned int)esp->fs);
    Print("  es=%x\n", (unsigned int)esp->es);
    Print("  ds=%x\n", (unsigned int)esp->ds);
    Print("  ebp=%x\n", (unsigned int)esp->ebp);
    Print("  edi=%x\n", (unsigned int)esp->edi);
    Print("  esi=%x\n", (unsigned int)esp->esi);
    Print("  edx=%x\n", (unsigned int)esp->edx);
    Print("  ecx=%x\n", (unsigned int)esp->ecx);
    Print("  ebx=%x\n", (unsigned int)esp->ebx);
    Print("  eax=%x\n", (unsigned int)esp->eax);
    Print("  intNum=%x\n", (unsigned int)esp->intNum);
    Print("  errorCode=%x\n", (unsigned int)esp->errorCode);
    Print("  eip=%x\n", (unsigned int)esp->eip);
    Print("  cs=%x\n", (unsigned int)esp->cs);
    Print("  eflags=%x\n", (unsigned int)esp->eflags);
    p = (void **)(((struct Interrupt_State *)esp) + 1);
    Print("esp+n=%x\n", (unsigned int)p);
    Print("esp+n[0]=%x\n", (unsigned int)p[0]);
    Print("esp+n[1]=%x\n", (unsigned int)p[1]);
}

void dump_stack(unsigned int *esp, unsigned int ofs) {
    int i;
    Print("Setup_Frame: Stack dump\n");
    for(i = 0; i < 25; i++) {
        Print("[%x]: %x\n", (unsigned int)&esp[i] - ofs, esp[i]);
    }
}
#endif

void Setup_Frame(struct Kernel_Thread *kthread,
                 struct Interrupt_State *state) {
    /*
     * This is called when Check Pending Signal returns true
     * for a process. It sets up the process’s user stack and
     * kernel stack so that when the process resumes execution,
     * it starts executing the correct signal handler, and when
     * that handler completes, the process will invoke the
     * trampoline function (which issues Sys ReturnSignal system
     * call). IF instead the process is relying on SIG IGN or SIG DFL,
     * handle the signal within the kernel. IF the process has defined
     * a signal handler for this signal, Setup Frame has to do the
     * following:
     *  1.  Choose the correct handler to invoke.
     *  2.  Acquire the pointer to the top of the user stack.
     *      This pointer is below the saved interrupt state stored
     *      on the kernel stack (as shown in the figure above).
     *  3.  Push onto the user stack a snapshot of the interrupt state
     *      that is currently stored at the top of the kernel stack.
     *      The interrupt state is the topmost portion of the kernel stack,
     *      defined in include/geekos/int.h in struct Interrupt State, shown above.
     *  4.  Push onto the user stack the number of the signal being delivered.
     *  5.  Push onto the user stack the address of the “trampoline”
     *      (which was registered by the Sys RegDeliver system call, mentioned above).
     *  6.  Change the current kernel stack such that (notice that you already
     *      saved a copy in the user stack)
     *      (a) The user stack pointer is updated to reflect the changes made in steps 3–5.
     *      (b) The saved program counter (eip) points to the signal handler.
     * */
    KASSERT(kthread);
    KASSERT(state);

    if (Interrupts_Enabled()){
        Print("Setup_Frame > Disabling interrupts\n");
        Disable_Interrupts();
    }

    Print("setup_frame > state->eax = %d \n", state->eax);

    //Print("KERNEL > Setup_Frame\n");
    int sig = kthread->userContext->signal;
    KASSERT(IS_SIGNUM(sig));

    signal_handler handler = kthread->userContext->handlers[kthread->userContext->signal];

    if (handler == Signal_Default || handler == Signal_Ignore) {
        //Print("Setup_Frame > A default handler for signal %d\n",sig);
        handler(sig);
        return;
    }

    // 2. Acquire pointer to top of user stack.
    struct User_Interrupt_State *user_interrupt_state = (struct User_Interrupt_State *)state;
    uint_t usr_sp = user_interrupt_state->espUser;

    // 3. Push onto the user stack a snapshot of the interrupt state
    usr_sp -= sizeof(struct Interrupt_State);
    Copy_To_User(usr_sp, state, sizeof(struct Interrupt_State));

    // 4. Push onto the user stack the number of the signal being delivered.
    usr_sp -= sizeof(int);
    Copy_To_User(usr_sp, &sig, sizeof(int));

    // 5. Push onto the user stack the address of the “trampoline”
    usr_sp -= sizeof(signal_handler*);
    Copy_To_User(usr_sp, &kthread->userContext->return_signal, sizeof(signal_handler));

    // 6. Change the current kernel stack
    state->eip = (uint_t )kthread->userContext->handlers[sig];
    user_interrupt_state->espUser = usr_sp;
    kthread->userContext->signal = 0;

}
