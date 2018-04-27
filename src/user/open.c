
#include <conio.h>
#include <process.h>
#include <fileio.h>
#include <string.h>
#include <geekos/errno.h>

int main(int argc, char *argv[]) {

    int retC;

    int fd = Open("/d/basic11d/f12", O_READ);
    if(fd < 0)
        return -1;

    retC = Close(fd);

    return (retC >= 0) ? 1 : -1;
}
