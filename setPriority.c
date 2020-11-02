#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

int 
main(int argc, char** argv)
{
   if (argc != 3)
   {
       printf(2, "Usage: setPriority <new-Priority> <pid for the process>");
       exit();
   } 
    
   int old = set_priority(atoi(argv[1]), atoi(argv[2]));
   printf(1, "Old: %d\nNew: %d\n", old, atoi(argv[2]));
   exit();
}