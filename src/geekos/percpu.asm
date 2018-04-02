	;;  percpu.asm redefines current_thread macros
    ;;  to use the per-cpu segment.


;; eax = *( &g_currentThreads[Get_CPU_ID()] )
;%macro Get_Current_Thread_To_EAX 0
;    Mov_EAX_Current_Thread_PTR
;    mov	eax, [eax]
;%endmacro
;
;%macro Set_Current_Thread_From_EBX 0
;    Mov_EAX_Current_Thread_PTR
;    mov	[eax], ebx
;%endmacro
;
;
;%macro Push_Current_Thread_PTR 0
;    Mov_EAX_Current_Thread_PTR
;    push	dword [eax]
;%endmacro

%macro Get_Current_Thread_To_EAX 0
    mov eax, [gs:0]
%endmacro

%macro Set_Current_Thread_From_EBX 0
    mov [gs:0], ebx
%endmacro

%macro Push_Current_Thread_PTR 0
    push	dword [gs:0]
%endmacro