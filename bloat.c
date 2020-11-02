#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

int 
main(int argc, char **argv)
{
    int rc = fork();

    if (rc < 0)
    {
        printf(2, "fork error: fork failed!\n");
        exit();
    }
    else if (rc == 0)
    {
        printf(1, "Bloat has started running!\n");
        double temp = 0.0;
        sleep(1);
        for (int i = 0; i < 30000000; i++)
            temp += 1.0 * 344.345645 * 234234459.2342341 ;
        printf(1, "Bloat has ended!\n");
    }
    else{
        int * wtime = malloc(sizeof(int));
        int * rtime = malloc(sizeof(int));
        waitx(wtime, rtime);
        printf(1, "Waiting time for the bloat = %d\nRunning time for the bloat = %d\n", *wtime, *rtime);
    }
    exit();
}