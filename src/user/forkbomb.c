#include <conio.h>
#include <process.h>

int main(void)
{
    while (Fork() != -1) {
        Print("Fork...\n");
        //void *a = Malloc(1024);
        //Print("a = %p\n", a);
    }
    Print("Ran out of memory...\n");
    return 0;
}