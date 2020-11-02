#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

#define RED "\u001b[31m"
#define RESET "\u001b[0m"

int 
main(int argc, char **argv)
{
    int rc = fork();

    if (rc < 0)
        printf(2, "fork error: fork failed!\n");
    else if (rc == 0){ 
        exec(argv[1], argv + 1);
        printf(2, "exec error: exec failed!\n");
    }
    else
    {
        int * rtime = malloc(sizeof(int));
        int * wtime = malloc(sizeof(int));
        
        waitx(wtime, rtime);
        // returns the clock cycle ticks.
        printf(1, "Waiting time for the process [pid: %d] = %d\nRunning time for the process [pid: %d] = %d\n", rc, *wtime, rc, *rtime);
    }
    exit();
}