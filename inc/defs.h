#pragma once
#include <cdefs.h>
#include <spinlock.h>

struct buf;
struct context;
struct extent;
struct inode;
struct proc;
struct rtcdate;
struct spinlock;
struct sleeplock;
struct stat;
struct superblock;
struct vpage_info;
struct vpi_page;
struct vregion;
struct vspace;


extern int npages;
extern int pages_in_use;
extern int pages_in_swap;
extern int free_pages;
extern int num_page_faults;
extern int num_disk_reads;

extern int crashn_enable;
extern int crashn;

// http://www.decompile.com/cpp/faq/file_and_line_error_string.htm
#define _xk_str(x) #x
#define __xk_str(x) _xk_str(x)

#define assert(x)		\
	do { \
    if (!(x)) \
      panic("assertion failed: '" #x "' at " __FILE__ ":" __xk_str(__LINE__)); \
  } while (0)

#define assertm(x, msg)		\
	do { \
    if (!(x)) \
      panic("assertion failed: '" msg "' at " __FILE__ ":" __xk_str(__LINE__)); \
  } while (0)

// bio.c
void binit(void);
struct buf *bread(uint, uint);
void brelse(struct buf *);
void bwrite(struct buf *);
void print_data_at_block(uint);

// console.c
void consoleinit(void);
void cprintf(char *, ...);
void consoleintr(int (*)(void));
noreturn void panic(char *);

// exec.c
int exec(char *, char **, int);

// fs.c
void readsb(int dev, struct superblock *sb);
struct inode *dirlookup(struct inode *, char *, uint *);
struct inode *rootlookup(char *);
struct inode *idup(struct inode *);
void iinit(int dev);
void irelease(struct inode *);
void locki(struct inode *);
void unlocki(struct inode *);
int namecmp(const char *, const char *);
struct inode *namei(char *);
struct inode *nameiparent(char *, char *);
int concurrent_readi(struct inode *, char *, uint, uint);
int readi(struct inode *, char *, uint, uint);
void concurrent_stati(struct inode *, struct stat *);
void stati(struct inode *, struct stat *);
int concurrent_writei(struct inode *, char *, uint, uint);
int writei(struct inode *, char *, uint, uint);

// ide.c
void ideinit(void);
void ideintr(void);
void iderw(struct buf *);

// ioapic.c
void ioapicenable(int irq, int cpu);
extern uchar ioapicid;
void ioapicinit(void);

// kalloc.c
struct core_map_entry *pa2page(uint64_t pa);
void detect_memory(void);
char *kalloc(void);
void kfree(char *);
void mem_init(void *);
void mark_user_mem(uint64_t, uint64_t);
void mark_kernel_mem(uint64_t);
struct core_map_entry *get_random_user_page();
void cmap_lock_init(void);

// kbd.c
void kbdintr(void);

// lapic.c
void cmostime(struct rtcdate *r);
int cpunum(void);
extern volatile uint *lapic;
void lapiceoi(void);
void lapicinit(void);
void lapicstartap(uchar, uint);
void microdelay(int);

// mp.c
extern int ismp;
void mpinit(void);

// vspace.c
void                vspacebootinit(void);
int                 vspaceinit(struct vspace *);
void                vspaceinitcode(struct vspace *, char *, uint64_t);
int                 vspaceloadcode(struct vspace *, char *, uint64_t *);
void                vspaceinvalidate(struct vspace *);
void                vspacemarknotpresent(struct vspace *, uint64_t);
void                vspaceinstall(struct proc *);
void                vspaceinstallkern(void);
void                vspacefree(struct vspace *);
struct vregion*     va2vregion(struct vspace *, uint64_t);
struct vpage_info*  va2vpage_info(struct vregion *, uint64_t);
int                 vregioncontains(struct vregion *, uint64_t, int);
int                 vspacecopy(struct vspace *, struct vspace *);
int                 vspaceinitstack(struct vspace *, uint64_t);
int                 vspacewritetova(struct vspace *, uint64_t, char *, int);
void                vspacedumpstack(struct vspace *);
void                vspacedumpcode(struct vspace *);
int                 vregionaddmap(struct vregion *, uint64_t, uint64_t, short, short);
int                 vregiondelmap(struct vregion *vr, uint64_t, uint64_t);
int                 vregiondelmap(struct vregion *, uint64_t, uint64_t);
void                vspacetordonly(struct vspace *);


// picirq.c
void picenable(int);
void picinit(void);

// proc.c
void exit(void);
int fork(void);
int growproc(int);
int kill(int);
void pinit(void);
void procdump(void);
noreturn void scheduler(void);
void sched(void);
void sleep(void *, struct spinlock *);
void userinit(void);
int wait(void);
void wakeup(void *);
void yield(void);
void reboot(void);
void update_rip(uint64_t);
void update_rdi(uint64_t new_rdi);
void update_rsp(uint64_t new_rsp);
void update_rsi(uint64_t new_rsi);


// swtch.S
void swtch(struct context **, struct context *);

// spinlock.c
void acquire(struct spinlock *);
void getcallerpcs(void *, uint64_t *);
int holding(struct spinlock *);
void initlock(struct spinlock *, char *);
void release(struct spinlock *);
void pushcli(void);
void popcli(void);

// sleeplock.c
void acquiresleep(struct sleeplock *);
void releasesleep(struct sleeplock *);
int holdingsleep(struct sleeplock *);
void initsleeplock(struct sleeplock *, char *);

// string.c
int memcmp(const void *, const void *, uint);
void *memmove(void *, const void *, uint);
void *memset(void *, int, uint);
char *safestrcpy(char *, const char *, int);
int strlen(const char *);
int strncmp(const char *, const char *, uint);
char *strncpy(char *, const char *, int);

// syscall.c
int argfd(int n, int *fd);
int argint(int, int *);
int argint64(int, int64_t *);
int argptr(int, char **, int);
int argstr(int, char **);
int fetchint(uint64_t, int *);
int fetchint64_t(uint64_t, int64_t *);
int fetchstr(uint64_t, char **);
void syscall(void);

// trap.c
void idtinit(void);
extern uint ticks;
void tvinit(void);
extern struct spinlock tickslock;

// uart.c
void uartinit(void);
void uartintr(void);
void uartputc(int);
// number of elements in fixed-size array
#define NELEM(x) (sizeof(x) / sizeof((x)[0]))


struct pipe {
  char* data; //this will be allocated kernel memory via kalloc
  int ref_count_readers; //number of times this pipe is referenced by rdonly 
  int ref_count_writers; //number of times this pipe is referenced by wronly
  char* read_cursor;
  char* write_cursor;
  int writer_exists; //to allow one write to finish before another begins
  int reader_exists; //to allow one read to finish before another begins
};


struct file_info {
  int in_mem_ref_count;
  struct inode* ref_inode;
  int current_offset;
  int permissions;
  int is_pipe; //0 if refers to inode, 1 if refers to pipe
  char* ref_pipe;
};

extern int first_file_allocated;

//helps for updating file records,
//at this point all files have
//contiguous extents, but from
//here on out that could change...
extern int updated_extra_extents_flag;

//struct ftable {
 // struct file_info data[16]; // <--- this 16 comes from NFILE, but I didn't want to redefine NFILE in case it clashes
 // struct spinlock lock;
//};

//extern struct ftable global_file_info_array;

extern struct file_info global_file_info_array[];

extern struct spinlock global_ftable_lock;


extern struct spinlock cmap_lock;


//sysfile.c
void update_global_table_on_fork(struct proc* child);
void close_all_fds_for_process(struct proc* exiting_proc);


extern struct pipe global_pipe_table[];


//extern int heap_cursor_initialized;


//extern int heap_cursor;



//for preventing two files from being created simulataneously
//from trying to allocate the same disk blocks
extern struct spinlock create_file_lock;
extern uint create_file_lock_in_use;
extern int create_file_lock_initialized;
