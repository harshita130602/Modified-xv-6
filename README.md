# Tweaking xv-6 
## By HARSHITA UPADHYAY (2019101126)

## Overview
Various improvements have been made to the xv6 operating system such as the waitx syscall and ps user program. Scheduling techniques such as FCFS, PBS, and MLFQ have also been implemented.

## Run the shell
* Run the following command 
```make && make qemu```
* Add the flag SCHEDULER to choose between RR, FCFS, PBS, and MLFQ as:
```make && make qemu SCHEDULER=RR```

# TASK 1
## Waitx systemcall
The following files have been modified:
* user.h
* usys.S
* syscall.h
* syscall.c
* sysproc.c
* defs.h
* proc.c
* proc.h

In the proc.c file, the waitx syscall has been added as a function given below:
``` C
int 
waitx(int* wtime, int* rtime)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // In order to scan through table looking for exited children.
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
        if (*wtime < 0)
            *wtime = 0;
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

    // if we don't have any children then there is no point in waiting.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }

}
```

### The Code for waitx() does the following:

* Search for a zombie child of parent in the proc table.
* When the child was found , following pointers were updated :
```
*wtime= p->etime - p->ctime - p->rtime - p->iotime;
*rtime=p->rtime;
```
* sysproc.c is just used to call waitx() which is present in proc.c. The sys_waitx() function in sysproc.c passes the parameters (rtime,wtime) to the waitx() of proc.c, just as other system calls do.

### Calculating ctime, etime and rtime
* ctime is recorded in allocproc() function of proc.c (When process is born). It is set to ticks.
* etime is recorded in exit() function (i.e when child exists, ticks are recorded) of proc.c.
* rtime is updated in trap() function of trap.c. IF STATE IS RUNNING , THEN UPDATE rtime.
* iotime is updated in trap() function of trap.c.(IF STATE IS SLEEPING , THEN UPDATE iotime.

### Tester file - time command
* time inputs a command and exec it normally
* Uses waitx instead of normal wait
* Displays wtime and rtime along with status that is the same as that returned by wait() syscall

## Ps user program
​ This user program returns some basic information about all the active processes. The code that has been added to proc.c is given below:
``` C
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
        int wtime = p->etime - p->ctime - p->rtime - p->iotime;
        if (p->etime == -1)
            wtime = ticks - p->ctime - p->rtime - p->iotime;
        if (wtime < 0)
            wtime = 0;
        cprintf("%d \t %d \t\t ", p->pid, p->priority);
        cprintf("%s \t %d \t\t ", states[p->state], p->rtime);
        cprintf("%d \t\t %d \t ",  wtime, p->n_run);
        cprintf("%d \t", p->cur_queue);
        for (int i = 0; i < MAXQUEUE; i++)
            cprintf(" %d \t", p->ticks[i]);
        cprintf("\n");
    }
    release(&ptable.lock);

    return 0;
}
```
### Explanation :

So, it is basically displaying all the basic info like : 

* Current priority of the process
* Current state of the process
* Total time for which the process ran on CPU till now
* Time for which the process has been waiting
* Number of times the process was picked by the scheduler
* Current queue
* Number of ticks the process has received at each of the 5 queues

# Task 2 
## First Come - First Serve (FCFS)
First Come First Serve (FCFS) is an operating system scheduling algorithm that automatically executes queued requests and processes in order of their arrival. It is the easiest and simplest CPU scheduling algorithm. In this type of algorithm, processes which requests the CPU first get the CPU allocation first. This is managed with a FIFO queue.

The code for it has been added to proc.c given below:

``` C
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

    #endif
```

### Explanation : 
This scheduler goes through all the process in queue and finds the one with least creation time for execution. If process with pid = 1 ( init ) or pid = 2 ( sh ) is encountered , it is executed directly.
Also in FCFS, the yield function is blocked which preempts the current running process and goes back to the scheduler.

## Priority based Scheduling (PBS)
Priority Scheduling is a method of scheduling processes that is based on priority. In this algorithm, the scheduler selects the tasks to work as per the priority. The processes with higher priority should be carried out first. The code for it has been added to proc.c given below: 

``` C 
    #ifdef PBS
    struct proc * minPriorityProc = NULL;
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
        if (p->state == RUNNABLE)
        {
            if (minPriorityProc == NULL)
            {
                minPriorityProc = p;
            }
            else
            {
                if (p->priority < minPriorityProc->priority
                        || (p->priority <= minPriorityProc && p->n_run <= minPriorityProc->n_run))
                {
                    minPriorityProc = p;
                }
            }
        }
    }
    if (minPriorityProc != NULL)
    {
        minPriorityProc->n_run += 1;

        c->proc = minPriorityProc;
        switchuvm(minPriorityProc);
        minPriorityProc->state = RUNNING;
        swtch(&(c->scheduler), minPriorityProc->context);
        switchkvm();

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
    }
    #endif
```

### Explanation :
The scheduler goes through all the processes in the queue and finds the minimum priority value ( Highest priority ). Then we loop through all the processes in ‘RUNNABLE’ state and execute only the ones with priority equal to the minimum value we found.

## Multi-level Feedback Queue (MLFQ)

In a multilevel queue-scheduling algorithm, processes are permanently assigned to a queue on entry to the system. Processes do not move between queues. The idea is to separate processes with different CPU-burst characteristics. If a process uses too much CPU time, it will be moved to a lower-priority queue. Similarly, a process that waits too long in a lower-priority queue may be moved to a higher-priority queue. This form of aging prevents starvation. 
The code for it has been added to proc.c file given below:

``` C
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

```

### EXPLANATION : 
In this scheduling algorithm we use FCFS for the first four queues and Round Robin in the last 5th queue. We push a forked process to the queue with highest priority ( Queue 1 ). The scheduler first checks the first queue for runnable processes and run them in a FCFS manner , then the second and so on till the 4th queue. Processes in the 5th queue are executed in a RR manner. After this another loop through all the processes check if the wait time in the queue has exceeded the queue wait limit or not. If yes, it pushes the process to the next higher priority queue. In trap.c where the clock is declared the yield function is also called when the process exceeds the time slice of that queue.If the process completely used the time slice then it is pushed to the next lower priority queue.
