
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

int 
main(int argc, char **argv)
{
    if (argc > 1){ 
        printf(2, "ps usage: ps\n");
        exit();
    }
    getps();
    exit();
}