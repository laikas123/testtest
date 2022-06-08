#include <cdefs.h>
#include <defs.h>
#include <file.h>
#include <fs.h>
#include <memlayout.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <spinlock.h>
#include <trap.h>
#include <x86_64.h>
#include <fs.h>
#include <file.h>
#include <vspace.h>

// process table
struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

// to test crash safety in lab5,
// we trigger restarts in the middle of file operations
void reboot(void) {
  uint8_t good = 0x02;
  while (good & 0x02)
    good = inb(0x64);
  outb(0x64, 0xFE);
loop:
  asm volatile("hlt");
  goto loop;
}

void pinit(void) { initlock(&ptable.lock, "ptable"); }

// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc *allocproc(void) {
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->killed = 0;

  release(&ptable.lock);

  // Allocate kernel stack.
  if ((p->kstack = kalloc()) == 0) {
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trap_frame *)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 8;
  *(uint64_t *)sp = (uint64_t)trapret;
  p -> heap_cursor = 0;
  sp -= sizeof *p->context;
  p->context = (struct context *)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->rip = (uint64_t)forkret;

  return p;
}

// Set up first user process.
void userinit(void) {
  struct proc *p;
  extern char _binary_out_initcode_start[], _binary_out_initcode_size[];

  p = allocproc();

  initproc = p;
  assertm(vspaceinit(&p->vspace) == 0, "error initializing process's virtual address descriptor");
  vspaceinitcode(&p->vspace, _binary_out_initcode_start, (int64_t)_binary_out_initcode_size);
  memset(p->tf, 0, sizeof(*p->tf));
  p -> heap_cursor = 0;
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ss = (SEG_UDATA << 3) | DPL_USER;
  p->tf->rflags = FLAGS_IF;
  p->tf->rip = VRBOT(&p->vspace.regions[VR_CODE]);  // beginning of initcode.S
  p->tf->rsp = VRTOP(&p->vspace.regions[VR_USTACK]);
  safestrcpy(p->name, "initcode", sizeof(p->name));

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);
  p->state = RUNNABLE;
  release(&ptable.lock);
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int fork(void) {

//  int pb = pages_in_use;



 // your code here
  //create the new child process
  struct proc* child = allocproc();


  //0 means the process wasn't created since 
  //no process will have pid = 0
  if(child == 0){
    return -1;
  }


  //set the parent via this processes pid
  child -> parent = myproc();

  //since the child will have the same heap as the parent
  //use the same heap cursor
  child -> heap_cursor = myproc() -> heap_cursor;

  //this means the child hasn't yet forked a child
  //so the limit needs to be established as where the 
  //heap currently is
  if(myproc() -> lower_lim_heap_cursor == -1){
    child -> lower_lim_heap_cursor = child -> heap_cursor; // <-- prevents freeing parent resources in sbrk (since it will get freed later by default)
    myproc() -> lower_lim_heap_cursor = child -> heap_cursor;
  //otherwise use the existing limit for all future children
  }else{
    child -> lower_lim_heap_cursor = myproc() -> lower_lim_heap_cursor;
  }

//  assertm(vspaceinit(&child->vspace) == 0, "error initializing process's virtual address descriptor");

  int check_vspace_init = vspaceinit(&child->vspace);

  if(check_vspace_init < 0){
    return -1;
  }


 // child -> vspace.regions[VR_CODE].size = myproc()
  //copy the virtual memory
  int check_vspace_copy = vspacecopy(&child -> vspace, &myproc()->vspace);


  //make sure the virtual memory was copied correctly
  if(check_vspace_copy == -1){
    return -1;
  }


  //need to reinstall the vspace since vspacecopy modified
  //the parent/child vspace and also called vspaceinvalidate
  vspaceinstall(myproc());



  for(int i = 0; i < NOFILE; i++){
    child -> proc_ptr_to_global_table[i] = &(*(myproc() -> proc_ptr_to_global_table[i]));
  }

  //increment the global counts for non NULL pointers
  update_global_table_on_fork(child);



  //copy the trap frame
  *child -> tf = *myproc() -> tf;

  //set return val to 0
  child -> tf -> rax = 0;

  //do this last because only now should the process be allowed to run
  child -> state = RUNNABLE;

//  cprintf("pages created my making process pid = %d is: %d \n", child -> pid, pages_in_use - pb);

  return child -> pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void exit(void) {
  struct proc *p;

  //check for children
  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p -> parent -> pid == myproc() -> pid){
      //if the child is still runnable 
      //make init its parent so it can 
      //stil run
      if(p -> state == RUNNABLE){
        //set parent to init (which must be the first process)
        p -> parent = &ptable.proc[0];
      }else if(p -> state == ZOMBIE){
        //free the kernel stack of the child
        kfree(p -> kstack);
        //free the virtual page table
        vspacefree(&p -> vspace);

        //set its state to UNUSED
        p -> state = UNUSED;
      //if the state is UNUSED this is likely just
      //extra junk lying around
      }else if(p -> state == UNUSED){
        continue;
      }else{
       // panic("can't handle this state in exit");
       p -> parent = &ptable.proc[0];
      }
    }
  }


//  acquire(&myproc() -> parent -> lock);
//  acquire(&myproc() -> lock);

  //close all file descriptors, updating the global file table
  //as well as updating any pipes that are referenced and closing
  //them if necessary
  close_all_fds_for_process(myproc());

  //set the state of the process to ZOMBIE
  //so the parent knows it can clean it up
  myproc() -> state = ZOMBIE;

  
  //wake any parents that were sleeping 
  //in the wait() function
  wakeup1((void*)myproc() -> parent -> pid);


//  release(&myproc() -> lock);

 // release(&myproc() -> parent -> lock);
  
  release(&ptable.lock);

 acquire(&ptable.lock);

  sched();

}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(void) {

  struct proc *p;
  // make sure there's at least one valid child
  int had_one_child = 0;

  //acquire lock on the process table
  acquire(&ptable.lock);



  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++){
     //if it's a child process that isn't in the UNUSED state
    if(p -> parent -> pid == myproc() -> pid && p -> state != UNUSED){
      //if the child is in the ZOMBIE state 
      //then that means it exited and the parent
      //should clean it up and also doesn't need to 
      //wait 

      had_one_child = 1;

      int return_pid = p -> pid;
      if(p -> state == ZOMBIE){
  //      int pb = pages_in_use;

        //free the childs kernel stack
        kfree(p -> kstack);
        //free the virtual page table
        vspacefree(&p -> vspace);

        //set the childs state to UNUSED freeing the proc struct
        p -> state = UNUSED;

//        cprintf("pages removed my making process pid = %d is: %d \n", p -> pid, pb-pages_in_use);

        //release all locks
        release(&ptable.lock);
        return return_pid;
       }
    }
  }





  //there was no children to wait for 
  //so return an error
  if(had_one_child == 0){
    release(&ptable.lock);
    return -1;
  }

  //this needs to be in an infinite loop,
  //because a process might wakeup due to
  //a request to kill the process, to make sure
  //the process goes back to sleep until it's
  //time to clean a zombie child make this an
  //infinite loop so that killed = 1 doesn't affect
  //the functionality of wait
  while(1){
    //set the channel to be the pid of this process since
    //both this process and its children can access it
    //set the lock to sleep on as a lock to this process
    sleep((void*)myproc() -> pid, &ptable.lock);

    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++){
     //if it's a child process
      if(p -> parent -> pid == myproc() -> pid){
        //if the child is in the ZOMBIE state
        //then that means it exited and the parent
        //should clean it up and also doesn't need to
        //wait
        int return_pid = p -> pid;
        if(p -> state == ZOMBIE){
//          int pb = pages_in_use;
          //free the childs kernel stack
          kfree(p -> kstack);
          //free the virtual page table
          vspacefree(&p -> vspace);


          //set the childs state to UNUSED freeing the proc struct
          p -> state = UNUSED;

  //        cprintf("pages removed my making process pid = %d is: %d \n",p -> pid, pb - pages_in_use);

          release(&ptable.lock);
          return return_pid;
         }
      }
    }

  }


 release(&ptable.lock);
 //shouldn't get here..
 return -1;
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void scheduler(void) {
  struct proc *p;

  for (;;) {
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
      if (p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      mycpu()->proc = p;
      vspaceinstall(p);
      p->state = RUNNING;
      swtch(&mycpu()->scheduler, p->context);
      vspaceinstallkern();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      mycpu()->proc = 0;
    }
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
void sched(void) {
  int intena;

  if (!holding(&ptable.lock))
    panic("sched ptable.lock");
  if (mycpu()->ncli != 1) {
    cprintf("pid : %d\n", myproc()->pid);
    cprintf("ncli : %d\n", mycpu()->ncli);
    cprintf("intena : %d\n", mycpu()->intena);

    panic("sched locks");
  }
  if (myproc()->state == RUNNING)
    panic("sched running");
  if (readeflags() & FLAGS_IF)
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&myproc()->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void) {
  acquire(&ptable.lock); // DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void forkret(void) {
  static int first = 1;

  vspaceinstall(myproc());

  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

/*
  if(myproc() -> pid == 126){

    char *argv[] = {"ls", 0};

    exec("ls", argv, 0);

  }
*/


 // release(&ptable.lock);


  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void sleep(void *chan, struct spinlock *lk) {
  if (myproc() == 0)
    panic("sleep");

  if (lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if (lk != &ptable.lock) { // DOC: sleeplock0
    acquire(&ptable.lock);  // DOC: sleeplock1
    release(lk);
  }

  // Go to sleep.
  myproc()->chan = chan;
  myproc()->state = SLEEPING;
  sched();

  // Tidy up.
  myproc()->chan = 0;

  // Reacquire original lock.
  if (lk != &ptable.lock) { // DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void wakeup1(void *chan) {
  struct proc *p;

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void wakeup(void *chan) {
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int kill(int pid) {
  struct proc *p;

  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->pid == pid) {
      p->killed = 1;
      // Wake process from sleep if necessary.
      if (p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void procdump(void) {
  static char *states[] = {[UNUSED] = "unused",   [EMBRYO] = "embryo",
                           [SLEEPING] = "sleep ", [RUNNABLE] = "runble",
                           [RUNNING] = "run   ",  [ZOMBIE] = "zombie"};
  int i;
  struct proc *p;
  char *state;
  uint64_t pc[10];

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->state == UNUSED)
      continue;
    if (p->state != 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if (p->state == SLEEPING) {
      getcallerpcs((uint64_t *)p->context->rbp, pc);
      for (i = 0; i < 10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

struct proc *findproc(int pid) {
  struct proc *p;
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->pid == pid)
      return p;
  }
  return 0;
}


void update_rip(uint64_t new_rip){


//  myproc() -> tf -> cs = 51;
  myproc() -> tf -> rip = new_rip;

}


void update_rdi(uint64_t new_rdi){

  myproc() -> tf -> rdi = new_rdi;


}


void update_rsp(uint64_t new_rsp){


  myproc() -> tf -> rsp = new_rsp;

}


void update_rsi(uint64_t new_rsi){

  myproc() -> tf -> rsi = new_rsi;

}

