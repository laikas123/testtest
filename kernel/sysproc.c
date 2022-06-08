#include <cdefs.h>
#include <date.h>
#include <defs.h>
#include <memlayout.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <x86_64.h>


//this will turn to 1 when the first process calls
//sbrk, and then the process initializes its heap
//cursor to its size 
//extern int heap_cursor_initialized = 0;
//extern int heap_cursor = 0;

int sys_crashn(void) {
  int n;
  if (argint(0, &n) < 0)
    return -1;

  crashn_enable = 1;
  crashn = n;

  return 0;
}

int sys_fork(void) { return fork(); }

void halt(void) {
  while (1)
    ;
}

void sys_exit(void) {
  // LAB2
  exit();
}

int sys_wait(void) { return wait(); }

int sys_kill(void) {
  int pid;

  if (argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int sys_getpid(void) { return myproc()->pid; }

int sys_sbrk(void) {
  // LAB3

  //set to 0 in userinit and allocproc (first and onward processes)
  //heap will never have cursor 0 since that's where code goes
  //so this is a decent security wise check
  if(myproc() -> heap_cursor == 0){
   // heap_cursor_initialized = 1;
    myproc() -> heap_cursor = myproc() -> vspace.regions[VR_HEAP].va_base;
    myproc() -> lower_lim_heap_cursor = -1;
    cprintf("my processes heap cursor is: %d \n", myproc() -> heap_cursor);
  }

  int size;

  //error check retrieve size
  if(argint(0, &size) < 0){
    return -1;
  }
//  if(size != 1){
//    cprintf("pid = %d \n", myproc() -> pid);
//    cprintf("size = %d \n", size);
//    cprintf("heap cursor = %d \n", myproc() -> heap_cursor);
//    cprintf("stack bottom - proposed heap = %d \n", myproc() -> vspace.regions[VR_USTACK].va_base - 10*PGSIZE - myproc() -> heap_cursor + size);
//    cprintf("heap base = %d \n", myproc() -> vspace.regions[VR_HEAP].va_base);
//  }


    //no point allocating zero or negative
  if(size == 0){
    return myproc() -> heap_cursor; 
  }else if(size > 0){

    //if for some reason it tries to write past the stacks limit there is not enough room...
    if( (myproc() -> heap_cursor + size) >= (myproc() -> vspace.regions[VR_USTACK].va_base - 10*PGSIZE)){
      return -1;
    }

    int free_pages_before = free_pages;

    //if the size is greater than the current need to add a page to size
    while(myproc() -> heap_cursor + size > myproc() -> vspace.regions[VR_HEAP].va_base + myproc() -> vspace.regions[VR_HEAP].size){
      myproc() -> vspace.regions[VR_HEAP].size = myproc() -> vspace.regions[VR_HEAP].size + PGSIZE;
    }


    struct vregion* vr_heap = &myproc() -> vspace.regions[VR_HEAP];


    if(vregionaddmap(vr_heap, myproc() -> heap_cursor, size, VPI_PRESENT, VPI_WRITABLE) < 0){
      return -1;
    }

    vspaceinvalidate(&myproc() -> vspace);
    vspaceinstall(myproc());

  //  if(size != 1){
    //  cprintf("pages allocated = %d \n", free_pages_before - free_pages);
   // }

    int old_size = myproc() -> heap_cursor;

    myproc() -> heap_cursor = myproc() -> heap_cursor + size;

    return old_size;

  //n is less than 0
  }else{

/*
    struct sys_info info1, info2;
    sysinfo(&info1);

    cprintf("pages_in_use before sbrk decrement = %d\n",
         info1.pages_in_use);
*/

    int abs_val = 0 - size;

    cprintf("lower limit = %d \n", myproc() -> lower_lim_heap_cursor);

    //if there isn't enough memory to deallocate treat as if sbrk(0) was called
    if(abs_val > (myproc() -> heap_cursor - myproc() -> vspace.regions[VR_HEAP].va_base) || abs_val > (myproc() -> heap_cursor - myproc() -> lower_lim_heap_cursor)){
      return myproc() -> heap_cursor;
    }else{

      struct vregion* vr_heap = &myproc() -> vspace.regions[VR_HEAP];

      int bytes_deleted = vregiondelmap(vr_heap, myproc() -> heap_cursor-1, abs_val);

      if(bytes_deleted != abs_val){
        return -1;
      }

      myproc() -> vspace.regions[VR_HEAP].size = myproc() -> vspace.regions[VR_HEAP].size - bytes_deleted;

      vspaceinvalidate(&myproc() -> vspace);
      vspaceinstall(myproc());


      int old_size = myproc() -> heap_cursor;

      myproc() -> heap_cursor = myproc() -> heap_cursor - abs_val;

/*
      sysinfo(&info1);

      cprintf("pages_in_use after sbrk decrement = %d\n",
         info1.pages_in_use);
*/

      return old_size;


    }

  }

}

int sys_sleep(void) {
  int n;
  uint ticks0;

  if (argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (myproc()->killed) {
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int sys_uptime(void) {
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
