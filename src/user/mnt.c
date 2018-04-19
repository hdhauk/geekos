

#include <conio.h>
#include <process.h>
#include <fileio.h>

int main(int argc, char *argv[]) {

    int rc = Mount("ide1", "/d", "gfs3");
    if(rc != 0) {
        Print("Mount failed: %s\n", Get_Error_String(rc));
        Exit(1);
    }
    Print("Mount returned %d.\n", rc);

    return 0;
}
