#include <cdefs.h>
#include <defs.h>
#include <memlayout.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <spinlock.h>
#include <trap.h>
#include <x86_64.h>

// Interrupt descriptor table (shared by all CPUs).
struct gate_desc idt[256];
extern void *vectors[]; // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

int num_page_faults = 0;

void tvinit(void) {
  int i;

  for (i = 0; i < 256; i++)
    set_gate_desc(&idt[i], 0, SEG_KCODE << 3, vectors[i], KERNEL_PL);
  set_gate_desc(&idt[TRAP_SYSCALL], 1, SEG_KCODE << 3, vectors[TRAP_SYSCALL],
                USER_PL);

  initlock(&tickslock, "time");
}

void idtinit(void) { lidt((void *)idt, sizeof(idt)); }

void trap(struct trap_frame *tf) {
  uint64_t addr;

  if (tf->trapno == TRAP_SYSCALL) {
    if (myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if (myproc()->killed)
      exit();
    return;
  }

  switch (tf->trapno) {
  case TRAP_IRQ0 + IRQ_TIMER:
    if (cpunum() == 0) {
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case TRAP_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case TRAP_IRQ0 + IRQ_IDE + 1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case TRAP_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case TRAP_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case TRAP_IRQ0 + 7:
  case TRAP_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n", cpunum(), tf->cs, tf->rip);
    lapiceoi();
    break;

  default:
    addr = rcr2();


    int solved_kernel_trap = 0;


    if(tf -> err == 3){

      //first figure out which region the address is in
      struct vregion* addr_region = va2vregion(&myproc() -> vspace, addr);

     // int was_killed = 0;

      //NULL check
      if(addr_region == (void*)0){
        myproc() -> killed = 1;
      }

      //then get the actual page info struct
      struct vpage_info* addr_info = va2vpage_info(addr_region, addr);

      //NULL check
      if(addr_info == (void*)0){
        myproc() -> killed = 1;
      }


      if(addr_info == (void*)0 || addr_info  ->  pre_cow_writable == !VPI_WRITABLE || addr_info -> used == 0 || addr_info -> present == !VPI_PRESENT){
      // Assume process misbehaved.
        cprintf("pid %d %s: trap %d err %d on cpu %d "
        "rip 0x%lx addr 0x%x--kill proc\n",
        myproc()->pid, myproc()->name, tf->trapno, tf->err, cpunum(),
        tf->rip, addr);
        myproc() -> killed = 1;
      //it's ok that this  page was written to make a copy of the physical page
      }else{


        //allocate memory for the new physical page
        char* mem = kalloc();

        if(!mem){
          cprintf("error couldn't kalloc enough memory, COW trap err\n");
          // myproc() -> killed = 1;  <-- not really sure what to do here...
        }else{
          //copy the contents of the page to the newly allocated memory
          memmove(mem, P2V(addr_info->ppn << PT_SHIFT), PGSIZE);

          //get the physical address
          uint64_t pa = (addr_info -> ppn << PT_SHIFT);

          //the physical page lost a reference since the writer is now going
          //to point to the newly created page, so decrement old pages ref count
          acquire(&cmap_lock);

          //get the corresponding core map entry
          struct core_map_entry* cmap_entry = pa2page(pa);

          //increase the reference count
          cmap_entry -> ref_count--;

          if(cmap_entry -> ref_count == 0){
            kfree(P2V(addr_info->ppn << PT_SHIFT));
          }

          release(&cmap_lock);

          addr_info->ppn = PGNUM(V2P(mem));
          addr_info->writable = VPI_WRITABLE;
          vspaceinvalidate(&myproc() -> vspace);
          vspaceinstall(myproc());

          solved_kernel_trap = 1;
      }



    }

  }

//    cprintf("tf -> err = %d, and addr = 0x%x \n", tf -> err, addr);


    if (tf->trapno == TRAP_PF) {
      num_page_faults += 1;

      if ( (myproc() == 0 || (tf->cs & 3) == 0) && solved_kernel_trap == 0 ) {

        // In kernel, it must be our mistake.
        cprintf("unexpected trap %d from cpu %d rip %lx (cr2=0x%x)\n",
                tf->trapno, cpunum(), tf->rip, addr);
        panic("trap");
      }
    }



    //check if the issue is a COW write
    //if b2 is set its user mode
    //if b1 it was from a write
    //if b0 set its a page protection issue
    //all of these signify write to read only
    //from user mode which relates to COW fork error
    if( (tf -> err) == 7){



      //first figure out which region the address is in
      struct vregion* addr_region = va2vregion(&myproc() -> vspace, addr);

     // int was_killed = 0;

      //NULL check
      if(addr_region == (void*)0){
        myproc() -> killed = 1;
      }

      //then get the actual page info struct
      struct vpage_info* addr_info = va2vpage_info(addr_region, addr);

      //NULL check
      if(addr_info == (void*)0){
        myproc() -> killed = 1;
      }

      //DEBUG
//      cprintf("cow write err with pid = %d \n", myproc() -> pid);

      //if it wasn't a writable page then this process should be killed
      //or if the page wasn't usable or present then also it should be killed
      if(addr_info == (void*)0 || addr_info  ->  pre_cow_writable == !VPI_WRITABLE || addr_info -> used == 0 || addr_info -> present == !VPI_PRESENT){
      // Assume process misbehaved.
        cprintf("pid %d %s: trap %d err %d on cpu %d "
        "rip 0x%lx addr 0x%x--kill proc\n",
        myproc()->pid, myproc()->name, tf->trapno, tf->err, cpunum(),
        tf->rip, addr);
        myproc() -> killed = 1;
      //it's ok that this  page was written to make a copy of the physical page
      }else{


        //allocate memory for the new physical page
        char* mem = kalloc();

        if(!mem){
          cprintf("error couldn't kalloc enough memory, COW trap err\n");
          // myproc() -> killed = 1;  <-- not really sure what to do here...
        }else{
          //copy the contents of the page to the newly allocated memory
          memmove(mem, P2V(addr_info->ppn << PT_SHIFT), PGSIZE);

          //get the physical address
          uint64_t pa = (addr_info -> ppn << PT_SHIFT);

          //the physical page lost a reference since the writer is now going 
          //to point to the newly created page, so decrement old pages ref count
          acquire(&cmap_lock);

          //get the corresponding core map entry
          struct core_map_entry* cmap_entry = pa2page(pa);

          //increase the reference count
          cmap_entry -> ref_count--;

          if(cmap_entry -> ref_count == 0){
            kfree(P2V(addr_info->ppn << PT_SHIFT));
          }

          release(&cmap_lock);



          //set the page info struct to point to the new physical page
          //and give write enable permission
          addr_info->ppn = PGNUM(V2P(mem));
          addr_info->writable = VPI_WRITABLE;
          vspaceinvalidate(&myproc() -> vspace);
          vspaceinstall(myproc());
        }
        
      }
    //assume it was stack if page wasn't present (either read or write)
    //but still check that it is in stack region below
    }else if((tf -> err) == 6 || (tf -> err) == 4 || (tf -> err) == 5){
      
      //I get this number based on vspaceinitstack
      //it calls vregionaddmap with start value as SZ_2G - 4096
      //which is really the same as SZ2G - 1 - 4095
      int base = SZ_2G - 1;
      int page_index = ((base - addr)/PGSIZE);
      //the start address of where the page should
      //be allocated
      int start_addr_alloc = base - page_index*PGSIZE - 4095;
      struct vregion *vr_stack = &myproc() -> vspace.regions[VR_USTACK];


      int exceeds_capacity = base - 1 - 9*PGSIZE - 4095;

      //don't allow stack to grow past 10 pages
      //don't allow the stack to grow upwards
      if(addr < exceeds_capacity || addr >= SZ_2G){
        myproc() -> killed = 1;
      }else{

        //OK this should generalize better... now it should only grow
        //size as needed (use page index + 1 since zero indexed) to check
        //and grow size
        if((1 + page_index) * PGSIZE > vr_stack -> size){
          vr_stack -> size = (1+ page_index )*PGSIZE;
        }

        if(myproc() -> stack_page_count <= 10){
          int addmap_code = vregionaddmap(vr_stack, start_addr_alloc,PGSIZE, VPI_PRESENT, VPI_WRITABLE);
          //0 or -1 would indicate an error here
          if(addmap_code == -1 || addmap_code == 0){

            // Assume process misbehaved.
            cprintf("pid %d %s: trap %d err %d on cpu %d "
            "rip 0x%lx addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno, tf->err, cpunum(),
            tf->rip, addr);
            myproc() -> killed = 1;
          }else{
            vspaceinvalidate(&myproc() -> vspace);
            vspaceinstall(myproc());
            myproc() -> stack_page_count++;
          }
        }else{

          // Assume process misbehaved.
          cprintf("pid %d %s: trap %d err %d on cpu %d "
            "rip 0x%lx addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno, tf->err, cpunum(),
            tf->rip, addr);
          myproc()->killed = 1;
        }
      }
    }else{
        if(solved_kernel_trap == 0){
          panic("can't handle this tf -> err...");
        }
    }
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if (myproc() && myproc()->killed && (tf->cs & 3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if (myproc() && myproc()->state == RUNNING &&
      tf->trapno == TRAP_IRQ0 + IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if (myproc() && myproc()->killed && (tf->cs & 3) == DPL_USER)
    exit();
}

