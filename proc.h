#include "param.h"

// Per-CPU state
struct cpu {
  uchar apicid;                // Local APIC ID
  struct context *scheduler;   // swtch() here to enter scheduler
  struct taskstate ts;         // Used by x86 to find stack for interrupt
  struct segdesc gdt[NSEGS];   // x86 global descriptor table
  volatile uint started;       // Has the CPU started?
  int ncli;                    // Depth of pushcli nesting.
  int intena;                  // Were interrupts enabled before pushcli?
  struct proc *proc;           // The process running on this cpu or null
};

extern struct cpu cpus[NCPU];
extern int ncpu;

//PAGEBREAK: 17
// Saved registers for kernel context switches.
// Don't need to save all the segment registers (%cs, etc),
// because they are constant across kernel contexts.
// Don't need to save %eax, %ecx, %edx, because the
// x86 convention is that the caller has saved them.
// Contexts are stored at the bottom of the stack they
// describe; the stack pointer is the address of the context.
// The layout of the context matches the layout of the stack in swtch.S
// at the "Switch stacks" comment. Switch doesn't save eip explicitly,
// but it is on the stack and allocproc() manipulates it.
struct context {
  uint edi;
  uint esi;
  uint ebx;
  uint ebp;
  uint eip;
};

enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
struct proc {
  uint sz;                     // Size of process memory (bytes)
  uint ctime;                  // Creation time for the process
  int etime;                  // End time for the process
  uint rtime;                  // total time for the process
  uint iotime;                 // total time spend in doing io
  uint priority;               // Priority for the current process
  pde_t* pgdir;                // Page table
  char *kstack;                // Bottom of kernel stack for this process
  enum procstate state;        // Process state
  int pid;                     // Process ID
  struct proc *parent;         // Parent process
  struct trapframe *tf;        // Trap frame for current syscall
  struct context *context;     // swtch() here to run process
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
  uint reset_ticks;            // Stores the last time process was scheduled
  int n_run;                   // Number of times the process is executed
  int cur_queue;               // Current queue
  int ticks[MAXQUEUE];         // Number of ticks the process receives at the `i`th queue
};

#ifdef MLFQ

typedef struct Queue {
    int beg,                   // Beginning index of the array
        end;                   // End index of the array (inclusive)
    struct proc *q[NUMQUEUE];  // Array of `struct proc` pointers
} Queue;
Queue mlfq[MAXQUEUE];

void pushback(Queue *, struct proc *);          // pushing the given process to the end of the Queue
void deletefront(Queue *);                      // Deleting the process with the given pid
void deleteIdx(Queue *, int);                   // Deleting the process for the given index in the Queue (for ageing of the processes)
int size(Queue *);                              // Returns size of the Queue
void ageproc(void);                             // Ageing the processes 

#endif
// Process memory is laid out contiguously, low addresses first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap