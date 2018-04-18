


#include <geekos/list.h>
#include <conio.h>

int main(){
    int *nullptr = (int *)NULL;
    Print("nullptr = 0x%x\n", (int)nullptr);
    Print("*nullptr = %d\n", *nullptr);
    return 0;
}