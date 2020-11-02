#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid   = nextpid++;
  p->ctime = ticks;
  p->etime = -1;
  p->rtime = 0;
  p->iotime = 0;
  p->priority = 60;                      // Default priority for a new process
  p->n_run = 0;
  p->reset_ticks = 0;
  p->cur_queue = -1;
  for (int i = 0; i < MAXQUEUE; i++)
    p->ticks[i] = -1;
  #ifdef MLFQ
  p->cur_queue = 0;
  for (int i = 0; i < MAXQUEUE; i++)
      p->ticks[i] = 0;
  #endif

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();

  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  curproc->etime = ticks;                       // update the ending time for the process
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    #ifdef RR
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    #else 
    #ifdef FCFS
    struct proc * firstComeProc = NULL;
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
        if (p->state == RUNNNABLE)
        {
            if (firstComeProc == NULL || firstComeProc->ctime > p->ctime)
                firstComeProc = p;
        }
    }
    // If no such process found, don't run the following
    if (firstComeProc != NULL)
    {
        c->proc = firstComeProc;
        switchuvm(firstComeProc);
        firstComeProc->state = RUNNING;

        swtch(&(c->scheduler), firstComeProc->context);
        switchkvm();

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
    }
    #else
    #ifdef PBS
    struct proc * minProc = NULL;
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
        if (p->state == RUNNABLE)
        {
            if (minProc == NULL)
            {
                minProc = p;
            }
            else
            {
                if (p->priority < minProc->priority
                        || (p->priority <= minProc && p->n_run <= minProc->n_run))
                {
                    minProc = p;
                }
            }
        }
    }
    if (minProc != NULL)
    {
        minProc->n_run += 1;

        c->proc = minProc;
        switchuvm(minProc);
        minProc->state = RUNNING;
        swtch(&(c->scheduler), minProc->context);
        switchkvm();

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
    }
    #else
    #ifdef MLFQ

    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
        if (p->state == RUNNABLE && p->queue == -1)    
        {
            pushback(&mlfq[0], p);
            p->queue = 0;
            p->reset_ticks = ticks;
        }
    }
    struct proc *toRun = NULL;
    
    for (int que = 0; que < MAXQUEUE; que++)
    {
        if (size(&mlfq[que]) > 0)
        {
            toRun = mlfq[que].q[mlfq[que].beg];

            if (toRun == NULL)
                continue;

            if (toRun->state == RUNNABLE)
                break;
            else
                deletefront(&mlfq[que]);
        }
    }
    if (toRun != NULL && toRun->state == RUNNABLE)
    {
        toRun->n_run += 1;
        toRun->reset_ticks = ticks;
        c->proc = toRun;
        switchuvm(toRun);
        toRun->state = RUNNING;

        swtch(&(c->scheduler), toRun->context);

        if (toRun->state == SLEEPING)
        {
            struct proc * del = mlfq[p->queue].q[q->beg];
            deletefront(&mlfq[p->queue]);
            pushback(&mlfq[p->queue], del);
        }

        switchkvm();

        c->proc = 0;
    }
    #endif
    #endif
    #endif
    #endif
    release(&ptable.lock);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == SLEEPING && p->chan == chan){
      p->state = RUNNABLE;
    #ifdef MLFQ
      q->reset_ticks = ticks;
      pushback(&mlfq[p->queue], p);
    #endif   
    }
  }
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      #ifdef MLFQ
      p->reset_ticks = ticks;
      pushback(&mlfq[p->queue], p);
      #endif
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
#ifdef MLFQ
    cprintf(" queue: %d", p->queue);
#endif
    cprintf("\n");
  }
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int 
waitx(int* wtime, int* rtime)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){

      if(p->parent != curproc)
        continue;

      havekids = 1;

      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        if (p->etime != -1)
            *wtime = p->etime - p->ctime - p->rtime - p->iotime;
        else 
            *wtime = ticks - p->ctime - p->rtime - p->iotime;
        *rtime = p->rtime;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }

}

int
getps(void)
{
    struct proc* p;
    char* states[] = { "UNUSED", "EMBRYO", "SLEEPING", "RUNNABLE", "RUNNING", "ZOMBIE" };

    acquire(&ptable.lock);
    cprintf("PID \t PRIORITY \t State \t\t r_time \t w_time \t n_run \t cur_q \t q0 \t q1 \t q2 \t q3 \t q4\n");
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
        if (p->pid <= 0)
            continue;
        // change the given wtime using reset_ticks
        int wtime = p->etime - p->ctime - p->rtime - p->iotime;
        if (p->etime < 0)
        {
            cprintf("Etime < 0.  Ticks : [%d]\n", ticks);
            wtime = ticks - p->ctime - p->rtime - p->iotime;
            if (wtime < 0)
                wtime = 0;
        }
        if (wtime < 0)
            cprintf("Etime: [%d]\tCtime: [%d]\tRtime: [%d]\tIOTime: [%d]\n", p->etime, p->ctime, p->rtime, p->iotime);
        #ifdef MLFQ
            wtime = ticks - p->reset_ticks;   
        #endif
        cprintf("%d \t %d \t\t ", p->pid, p->priority);
        cprintf("%s \t %d \t\t ", states[p->state], p->rtime);
        cprintf("%d \t\t %d \t ", wtime, p->n_run);
        cprintf("%d \t", p->cur_queue);
        for (int i = 0; i < MAXQUEUE; i++)
            cprintf(" %d \t", p->ticks[i]);
        cprintf("\n");
    }
    release(&ptable.lock);

    return 0;
}

int 
set_priority(int new_priority, int pid)
{
    struct proc* p;
    int old_priority = -1;
    if (new_priority < 0 || new_priority > 100)
    {
        cprintf("<new-priority> for the process should be between 0-100!\n");
        return -1;
    }
	acquire(&ptable.lock);
	for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
	  if(p->pid == pid){
            old_priority = p->priority;
			p->priority = new_priority;
			break;
		}
	}
	release(&ptable.lock);
    #ifdef PBS
    if (old_priority < new_priority)
        yield();
    #endif
	return old_priority;
}

#ifdef MLFQ

void pushback(Queue *q, struct proc *p)
{
    if (!q)
        panic("Invalid Queue given!\n");
    if (!p)
        panic("Invalid Process given!\n");

    if (size(q) == NUMQUEUE) 
        panic("The Queue is full!\n")

     if (q->beg > 0)
        for (int i = q->beg; i != (q->end + 1) % NUMQUEUE; i++, i %= NUMQUEUE)
            if (q->q[i] != NULL && q->q[i]->pid == p->pid)
                return;
    
    if (q->beg == -1)
    {
        q->q[0] = p;
        q->beg = q->end = 0;
    }
    else if (q->end == NUMQUEUE - 1 && q->beg != 0)
    {
        q->end = 0;
        q->q[0] = p;
    }
    else
    {
        q->end++;
        q->q[q->end] = p;
    }
}

void deletefront(Queue *q)
{
    if (!q)
        panic("Invalid Queue given!\n");

    if (q->beg == -1)
        panic("Deleting from empty queue!\n");

    if (q->beg == q->end)
        q->end = q->beg = -1;
    else if (q->beg == NUMQUEUE - 1)
        q->beg = 0;
    else
        q->beg++;
}

void deleteIdx(Queue *q, int idx)
{
    if (idx < q->beg || idx > q->end)
        panic("Invalid idx!\n");

    for (int i = idx; i != (q->end + 1) % NUMQUEUE; i++, i %= NUMQUEUE)
       q->q[i] = q->q[(j + 1) % NUMQUEUE];

    if (q->beg == q->end)
        q->beg = q->end = -1;
    else if (q->end == 0 && q->beg > q->end)
        q->end = NUMQUEUE - 1;
    else
        q->end--;
}

int size(Queue *q)
{
    if (q == NULL)
        panic("Invalid Queue given!\n");

    if (q->beg == -1)
        return 0;
    
    if (q->end >= q->beg)
        return q->end - q->beg + 1;

    return NUMQUEUE + q->end - q->beg;
}

void 
ageproc(void)
{
    for (int que = 0; que < MAXQUEUE; que++)
    {
        if (size(&mlfq[que]))
        {
            Queue *q = &mlfq[que];
            
            for (int i = q->beg; i != (q->end + 1) % NUMQUEUE; i++, i %= NUMQUEUE)
            {
                if (q->q[i] && ticks - q->reset_ticks > AGE)
                {
                    if (q->q[i]->queue < 0)
                        continue;
                    int newQ = q->q[i]->queue - 1;
                    if (newQ < 0)
                        newQ = 0;

                    struct proc *del = mlfq[q->q[i]->queue].q[i];
                    deleteIdx(&mlfq[q->q[i]->queue], i);

                    del->reset_ticks = ticks;
                    del->queue = newQ;

                    pushback(&mlfq[newQ], del);
                }
            }
        }
    }
}

#endif