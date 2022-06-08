//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include <cdefs.h>
#include <defs.h>
#include <fcntl.h>
#include <file.h>
#include <fs.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <sleeplock.h>
#include <spinlock.h>
#include <stat.h>
#include <vspace.h>

#include <buf.h>

#define NULL ((void *)0)




//struct ftable global_file_info_array;

struct file_info global_file_info_array[NFILE];

struct spinlock global_ftable_lock;



//mark that no files have been allocated
//yet so it's a good idea to "zero"
//the global array (and process table) in case random junk
//looks like something useful
int first_file_allocated = 0;



//has to do with marking metadata of extra extents for file
int updated_extra_extents_flag = 0;



//for kernel buffer of pipes
////actual size of buffer is 4KB but to avoid edge cases
//leave some breather room by considering it only 3900 bytes

int MAX_CURSOR_VALUE = 3900;

/*
 * arg0: int [file descriptor]
 *
 * duplicate the file descriptor arg0, must use the smallest unused file descriptor
 * returns a new file descriptor of the duplicated file, -1 otherwise
 *
 * dup is generally used by the shell to configure stdin/stdout between
 * two programs connected by a pipe (lab 2).  For example, "ls | more"
 * creates two programs, ls and more, where the stdout of ls is sent
 * as the stdin of more.  The parent (shell) first creates a pipe
 * creating two new open file descriptors, and then create the two children.
 * Child processes inherit file descriptors, so each child process can
 * use dup to install each end of the pipe as stdin or stdout, and then
 * close the pipe.
 *
 * Error conditions:
 * arg0 is not an open file descriptor
 * there is no available file descriptor
 */

int sys_dup(void) {

  //no files have even been opened so must be error
  if(first_file_allocated == 0){
    return -1;
  }

  //for getting file descriptor arg
  int fd;

  //check valid_fd
  int valid_fd = argfd(0, &fd);

  if(valid_fd < 0){
    return -1;
  }

  acquire(&global_ftable_lock);

  //find the first open value in the per
  //process table and set it's pointer accordingly
  for(int i = 0; i < NOFILE; i++){

      if(myproc() -> proc_ptr_to_global_table[i] == NULL ){
        myproc() -> proc_ptr_to_global_table[i] = myproc() -> proc_ptr_to_global_table[fd];
        //increment the in mem ref count since now the process has an additional pointer to it
        myproc() -> proc_ptr_to_global_table[i] -> in_mem_ref_count = myproc() -> proc_ptr_to_global_table[i] -> in_mem_ref_count + 1;

        if(myproc() -> proc_ptr_to_global_table[i] -> is_pipe){

          struct pipe* pipe_to_edit = ((struct pipe*)(myproc() -> proc_ptr_to_global_table[i] -> ref_pipe));

          if(myproc() ->  proc_ptr_to_global_table[i] -> permissions == O_RDONLY){
            pipe_to_edit -> ref_count_readers = pipe_to_edit -> ref_count_readers + 1;
          }else{
            pipe_to_edit -> ref_count_writers = pipe_to_edit -> ref_count_writers + 1;
          }

        }

        release(&global_ftable_lock);

        //this i value is the lowest available file descriptor so return it
        return i;
      }
    }

  release(&global_ftable_lock);


  //no valid value was found

  return -1;
}




void pipe_check_if_read_needs_sleep(struct pipe* active_pipe){
  //if the read and write cursrors equal each other then a read
  //cannot occur because that means this pipe hasn't yet been written
  //to since this only is possible for a fresh pipe
  //
  //the other edge case is make sure there is always a gap
  //of 1 between the reader and the writer that is that the reader cursor
  //can read a byte and move forward by at least 1 and won't overlap
  //the writer cursor (in simpler terms write cursor must always lead read cursor by 2)
  if( (active_pipe -> read_cursor == active_pipe -> write_cursor) || ((active_pipe -> read_cursor < active_pipe -> write_cursor) && (active_pipe -> write_cursor - active_pipe -> read_cursor <= 0)) || (active_pipe -> read_cursor == active_pipe -> data + sizeof(struct pipe) +  MAX_CURSOR_VALUE && active_pipe -> write_cursor == active_pipe -> data + sizeof(struct pipe)) ){

    while((active_pipe -> read_cursor == active_pipe -> write_cursor) || ((active_pipe -> read_cursor < active_pipe -> write_cursor) && (active_pipe -> write_cursor - active_pipe -> read_cursor <= 0)) || (active_pipe -> read_cursor == active_pipe -> data + sizeof(struct pipe) +  MAX_CURSOR_VALUE && active_pipe -> write_cursor == active_pipe -> data + sizeof(struct pipe)) ){
      //sleep until there is room to read, by using a pointer to the pipe
      //as the chan value this means any other processes that have access
      //to the pipe can also wakeup on the same chan so a deadlock won't happen
      sleep(active_pipe -> data, &global_ftable_lock);
    }
  }
}


/*
 * arg0: int [file descriptor]
 * arg1: char * [buffer to write read bytes to]
 * arg2: int [number of bytes to read]
 *
 * reads up to arg2 bytes from the current position of the file descriptor
 * arg0 and places those bytes into arg1. The current position of the
 * file descriptor is updated by that number of bytes.
 *
 * returns number of bytes read, or -1 if there was an error.
 *
 * If there are insufficient available bytes to complete the request,
 * reads as many as possible before returning with that number of bytes.
 *
 * Fewer than arg2 bytes can be read in various conditions:
 * If the current position + arg2 is beyond the end of the file.
 * If this is a pipe or console device and fewer than arg2 bytes are available
 * If this is a pipe and the other end of the pipe has been closed.
 *
 * Error conditions:
 * arg0 is not a file descriptor open for read
 * some address between [arg1,arg1+arg2-1] is invalid
 * arg2 is not positive
 */

int sys_read(void) {

  //no files have even been opened so must be error
  if(first_file_allocated == 0){
    return -1;
  }

  //for getting file descriptor arg
  int fd;

  //the buffer to read into
  char* buf;

  //get the number of bytes to read
  int size;

  acquire(&global_ftable_lock);


  //check valid_fd
  int valid_fd = argfd(0, &fd);

  if(valid_fd < 0){

    release(&global_ftable_lock);

    return -1;
  }


  //check that the permission of the file isn't write only
  if(myproc() -> proc_ptr_to_global_table[fd] ->permissions == O_WRONLY){
    release(&global_ftable_lock);

    return -1;
  }

  //check valid argint
  int err_code_argint2 = argint(2, &size);

  if(err_code_argint2 < 0){
    release(&global_ftable_lock);

    return -1;
  }

  //negative size shouldn't be valid
  if(size < 0){
    release(&global_ftable_lock);
    return -1;
  }

  //check valid ptr retrieved
  int err_code_argptr = argptr(1, &buf, size);

  if(err_code_argptr < 0){
    release(&global_ftable_lock);

    return -1;
  }


//dealing with inode
if(myproc() -> proc_ptr_to_global_table[fd] -> is_pipe == 0){



    //get pointer to inode referenced b the file descriptor
    struct inode* fd_inode = myproc() -> proc_ptr_to_global_table[fd] -> ref_inode;


    //if for some reason the inode isn't valid
    //this should yield error
    if(fd_inode == NULL){
      release(&global_ftable_lock);

      return -1;
    }

    //for T_DEV do same as always
    if(fd_inode -> type == T_DEV){

      //get the current offset
      int offset = myproc() -> proc_ptr_to_global_table[fd] -> current_offset;

      release(&global_ftable_lock);


      //read the bytes
      int bytes_read = concurrent_readi(fd_inode, buf, offset, size);

      acquire(&global_ftable_lock);


      //if the read was successful increment the offset appropriately
      //for the global file struct
      if(bytes_read > 0){
        myproc() -> proc_ptr_to_global_table[fd] -> current_offset = myproc() -> proc_ptr_to_global_table[fd] -> current_offset + bytes_read;
      }

      release(&global_ftable_lock);

      return bytes_read;

    //read fromt the disk blocks of a file
    }else{

      //get the current offset
      int offset = myproc() -> proc_ptr_to_global_table[fd] -> current_offset;

      //need to release spinlock so it's possible to call bread which uses sleeplock
      release(&global_ftable_lock);



      locki(fd_inode);
      //set to error so if something goes wrong it's obvious
      int bytes_read = -1;

      offset = myproc() -> proc_ptr_to_global_table[fd] -> current_offset;

      struct buf* bp;

      int start_block = fd_inode -> data.startblkno+offset/BSIZE;
      int end_block = fd_inode -> data.startblkno+(offset + size)/BSIZE;


      //if trying to read past the end of the file only read to end of file
      if(size > (fd_inode -> size - offset)) {
        size = fd_inode -> size - offset;
      }

      if(size == 0){
        unlocki(fd_inode);
        return 0;
      }

      int current_block = start_block;

      int bytes_left_to_read = size;

      int start_block_extra_extents = fd_inode -> extra_extents_blk_nums[27];

      int block_pos = offset%BSIZE;

      while(bytes_left_to_read > 0){

        //all blocks are contiguous
        if(start_block_extra_extents == -1){

          bp = bread(fd_inode -> dev, current_block);

          for(int i = block_pos; i < BSIZE; i++){
            buf[size - bytes_left_to_read] = bp -> data[i];

            bytes_left_to_read--;

            if(bytes_left_to_read == 0){
              break;
            }

          }

          //reset to start for each new block
          block_pos = 0;

          current_block++;

          //only need release since nothing modified on read
          brelse(bp);

          

        //some blocks might not be contiguous
        }else{
          panic("this need to be implemeneted (sys_read)");
        }

      }

      bytes_read = size;

      acquire(&global_ftable_lock);
      if(bytes_read > 0){
        myproc() -> proc_ptr_to_global_table[fd] -> current_offset = myproc() -> proc_ptr_to_global_table[fd] -> current_offset + bytes_read;
      }
      release(&global_ftable_lock);

      unlocki(fd_inode);

     // acquire(&global_ftable_lock);
     // if(bytes_read > 0){
       // myproc() -> proc_ptr_to_global_table[fd] -> current_offset = myproc() -> proc_ptr_to_global_table[fd] -> current_offset + bytes_read;
    //  }
     // release(&global_ftable_lock);

      return bytes_read;

    }

  //dealing with pipe
  }else{


//    char** active_pipe_char = myproc() -> proc_ptr_to_global_table[fd] -> ref_pipe;

    struct pipe* active_pipe = ((struct pipe*)myproc() -> proc_ptr_to_global_table[fd] -> ref_pipe);


    if(active_pipe -> reader_exists == 1){
      while(active_pipe -> reader_exists == 1){
        sleep(&active_pipe, &global_ftable_lock);
      }
    }
    //make this process the active reader
    active_pipe -> reader_exists = 1;

    int bytes_read = 0;

    //if there are no active writers read whatever is left in the pipe (if anything)
    if(active_pipe -> ref_count_writers == 0) {
      //if the cursors are equal this means the pipe was never written to or
      //read from since a gap is enforced after the first write/read
      //therefore return 0
      if(active_pipe -> read_cursor == active_pipe -> write_cursor){
        release(&global_ftable_lock);
        active_pipe -> reader_exists = 0;
        return bytes_read;
        //here it's ok if the read cursor overlaps the write cursor
        //because the pipe will be closed after the read
      }else{
        for(int i = 0; i < size; i++){
          buf[i] = *active_pipe -> read_cursor;
          bytes_read = bytes_read + 1;
          //if the cursor is at the end of the buffer
          //reset it to the beginning
          if(active_pipe -> read_cursor == active_pipe -> data + sizeof(struct pipe) + MAX_CURSOR_VALUE){
            active_pipe -> read_cursor = active_pipe -> data + sizeof(struct pipe); 
          }else{
            active_pipe -> read_cursor = active_pipe -> read_cursor + 1;
          }
          //if the read cursor = the write cursor there is no more bytes to read
          //or if this is the last bytes requested by the user then return
          if(active_pipe -> read_cursor == active_pipe -> write_cursor || i == size - 1){
            release(&global_ftable_lock);
            active_pipe -> reader_exists = 0;
            return bytes_read;
          }
        }
      }

    }

    //check if we should sleep
    pipe_check_if_read_needs_sleep(active_pipe);


    //begin reading from the pipe
    for(int i = 0; i < size; i++){
      buf[i] = *active_pipe -> read_cursor;
      bytes_read = bytes_read + 1;
      //if the cursor is at the end of the buffer
      //reset it to the beginning
      if(active_pipe -> read_cursor == active_pipe -> data + sizeof(struct pipe) + MAX_CURSOR_VALUE){
        active_pipe -> read_cursor = active_pipe -> data + sizeof(struct pipe);
      //otherwise just increment
      }else{
        active_pipe -> read_cursor = active_pipe -> read_cursor + 1;
      }
      //if the read cursor = the write cursor there is no more bytes to read
      //or if this is the last bytes requested by the user then return
      if(i == size - 1 || active_pipe -> ref_count_writers == 0 ){
        break;
      }else{
        //check if needs to sleep
        pipe_check_if_read_needs_sleep(active_pipe);
      }

    }
    //let other processes become the readers since we are done
    active_pipe -> reader_exists = 0;

    //wakeup any writers sleeping on the pipe
    wakeup(active_pipe -> data);


    release(&global_ftable_lock);
    return bytes_read;

  }



}


//checks if the writer to a pipe should sleep
//if it does it loops until it doesn't need to sleep
void pipe_check_if_write_needs_sleep(struct pipe* active_pipe){



  if( ( (active_pipe -> write_cursor < active_pipe -> read_cursor) && ( active_pipe -> read_cursor - active_pipe -> write_cursor) <= 1) || (active_pipe -> read_cursor == active_pipe -> data + sizeof(struct pipe) && active_pipe -> write_cursor == active_pipe -> data + sizeof(struct pipe) + MAX_CURSOR_VALUE)){
    //for my implementation to avoid lapping the read cursor
    //the write cursor must always remain at least 1 behind
    //the read cursor, that is assuming write_cursor < read_cursor
    //if write_cursor - read_cursor <= 1 then no writes are allowed
    //because the buffer is flow and needs to block
    while( ( (active_pipe -> write_cursor < active_pipe -> read_cursor) && ( active_pipe -> read_cursor - active_pipe -> write_cursor) <= 1) || (active_pipe -> read_cursor == active_pipe -> data + sizeof(struct pipe) && active_pipe -> write_cursor == active_pipe -> data + sizeof(struct pipe) + MAX_CURSOR_VALUE)  ){
      //sleep until there is room to write, by using a pointer to the pipe
      //as the chan value this means any other processes that have access
      //to the pipe can also wakeup on the same chan so a deadlock won't happen
      sleep(active_pipe -> data, &global_ftable_lock);
    }
  }
}


//  return 0;


/*
 * arg0: int [file descriptor]
 * arg1: char * [buffer of bytes to write to the given fd]
 * arg2: int [number of bytes to write]
 *
 * writes up to arg2 bytes from arg1 to the current position of
 * the file descriptor. The current position of the file descriptor
 * is updated by that number of bytes.
 *
 * returns number of bytes written, or -1 if there was an error.
 *
 * If the full write cannot be completed, writes as many as possible
 * before returning with that number of bytes. For example, if the disk
 * runs out of space.
 *
 * If writing to a pipe and the other end of the pipe is closed,
 * will return -1.
 *
 * Error conditions:
 * arg0 is not a file descriptor open for write
 * some address between [arg1,arg1+arg2-1] is invalid
 * arg2 is not positive
 *
 * note that for lab1, the file system does not support writing past
 * the end of the file. Normally this would extend the size of the file
 * allowing the write to complete, to the maximum extent possible
 * provided there is space on the disk.
 */

int sys_write(void) {

//  create_file_with_given_name("test100.txt");

//  print_bitmap_block(28);
  // cprintf("GOT HERE\n");

  if(updated_extra_extents_flag == 0){

    struct buf* check_created;

    check_created = bread(1, 33);

    struct dinode* dinode_list = (struct dinode*)(check_created -> data);

    if(dinode_list[0].type == 2 && dinode_list[0].size == 20){
      brelse(check_created);
      update_extra_extents_flag();
      updated_extra_extents_flag = 1;
    }else{
      brelse(check_created);
    }


  }

  //no files have even been opened so must be error
  if(first_file_allocated == 0){

    return -1;
  }
  // you have to change the code in this function.
  // Currently it supports printing one character to the screen.

  //for getting file descriptor arg
  int fd;

  //the buffer to use for writing from
  char* buf;

  //get the number of bytes to write
  int size;

  acquire(&global_ftable_lock);


  //check valid_fd
  int valid_fd = argfd(0, &fd);

  if(valid_fd < 0){

    release(&global_ftable_lock);
    cprintf("err 1\n");
    return -1;
  }

  //check that the permission of the file isn't read only
  if(myproc() -> proc_ptr_to_global_table[fd] ->permissions == O_RDONLY){
    release(&global_ftable_lock);
    cprintf("err 2\n");
    return -1;
  }
  //check valid arg int
  int err_code_argint2 = argint(2, &size);

  if(err_code_argint2 < 0){

    release(&global_ftable_lock);
    cprintf("err 3\n");
    return -1;
  }

  //negative size shouldn't be valid
  if(size < 0){

    release(&global_ftable_lock);
    cprintf("err 4\n");
    return -1;
  }
  //check valid pointer retrieved
  int err_code_argptr = argptr(1, &buf, size);

  if(err_code_argptr < 0){

    release(&global_ftable_lock);
    cprintf("err 5\n");
    return -1;
  }


  //writing to an inode
  if(myproc() -> proc_ptr_to_global_table[fd] -> is_pipe == 0){

    //get the inode based on fd
    struct inode* fd_inode = myproc() -> proc_ptr_to_global_table[fd] -> ref_inode;

    //if for some reason the inode isn't valid
    //this should yield error
    if(fd_inode == NULL){

      release(&global_ftable_lock);
      cprintf("err 6\n");
      return -1;
    }




    //get the offset
    int offset = myproc() -> proc_ptr_to_global_table[fd] -> current_offset;


    int bytes_written;

    if(fd_inode -> type == T_DEV){
      //write the bytes
      bytes_written = concurrent_writei(fd_inode, buf, offset, size);

      if(bytes_written > 0){
        myproc() -> proc_ptr_to_global_table[fd] -> current_offset = myproc() -> proc_ptr_to_global_table[fd] -> current_offset + bytes_written;
      }


    }else{
      //set to error code initially so if bytes aren't written it fails
      bytes_written = -1;


      struct buf* bp;
      //need to release spinlock before acquiring sleeplock on buf
      release(&global_ftable_lock);

      locki(fd_inode);

      offset = myproc() -> proc_ptr_to_global_table[fd] -> current_offset;

      //check the dinode size is capable of holding the data
      struct dinode fd_dinode;
      read_dinode(fd_inode -> inum, &fd_dinode);


      int start_block = fd_inode -> data.startblkno+offset/BSIZE;
      int end_block = fd_inode -> data.startblkno+(offset + size)/BSIZE;


      //check if writing past number of blocks allocated for file
      if(end_block > fd_inode -> data.startblkno + fd_inode -> data.nblocks - 1) {
        //TODO if we are supposed to offer allocating
        //more blocks to exisiting files here is where
        //it can be done
        unlocki(fd_inode);
        cprintf("inum of error = %d \n", fd_inode -> inum);
        panic("too much data written to file");
      }else{

        int size_to_add_to_dinode = 0;

        if((offset+size) > fd_dinode.size){
          size_to_add_to_dinode = (offset+size) - fd_dinode.size;
        }

        fd_dinode.size += size_to_add_to_dinode;

        int done_writing = 0;

        int current_block = start_block;

        int bytes_left_to_write = size;

        int start_block_extra_extents = fd_inode -> extra_extents_blk_nums[27];

        int block_pos = offset%BSIZE;

        while(bytes_left_to_write > 0){

          //all extents are contiguous
          if(start_block_extra_extents == -1){
            bp = bread(fd_inode -> dev, current_block);


            for(int i = block_pos; i < BSIZE; i++){
              bp -> data[i] = buf[size - bytes_left_to_write];
              bytes_left_to_write--;
              if(bytes_left_to_write == 0){
                break;
              }
            }

            current_block++;

            //reset to 0 for each new block
            block_pos = 0;

            bwrite(bp);
            brelse(bp);

          //some extents might not be contiguous
          }else{
            panic("this is tested so need to implement");
          }

        }

        bytes_written = size;

        write_dinode(fd_inode -> inum, fd_dinode);

        //need to reacquire lock to update offset in global table
        acquire(&global_ftable_lock);
        if(bytes_written > 0){
          myproc() -> proc_ptr_to_global_table[fd] -> current_offset = myproc() -> proc_ptr_to_global_table[fd] -> current_offset + bytes_written;
        }
        release(&global_ftable_lock);


        unlocki(fd_inode);

        fd_inode -> valid = 0;
        locki(fd_inode);
        unlocki(fd_inode);


      }


      //need to reacquire lock to update offset in global table
     // acquire(&global_ftable_lock);
     // if(bytes_written > 0){
      //  myproc() -> proc_ptr_to_global_table[fd] -> current_offset = myproc() -> proc_ptr_to_global_table[fd] -> current_offset + bytes_written;
     // }
     // release(&global_ftable_lock);

      return bytes_written;

    }


    release(&global_ftable_lock);


    return bytes_written;
 //writing to a pipe
 }else{
//    struct pipe* active_pipe = myproc() -> proc_ptr_to_global_table[fd] -> ref_pipe;
  //  char** active_pipe_char = myproc() -> proc_ptr_to_global_table[fd] -> ref_pipe;

//    struct pipe* active_pipe = ((struct pipe*)(&((*active_pipe_char)[0])));
    struct pipe* active_pipe = ((struct pipe*)myproc() -> proc_ptr_to_global_table[fd] -> ref_pipe);

    //if there are no active readers of this pipe return -1
    //because that's an error
    if(active_pipe -> ref_count_readers == 0){
      release(&global_ftable_lock);
      cprintf("err 7\n");
      return -1;
    }
    //if another writer has already began it's write process
    //make this writer sleep until that writer is finished
    if(active_pipe -> writer_exists == 1){
      while(active_pipe -> writer_exists == 1){
        sleep(&active_pipe, &global_ftable_lock);
      }
    }


    //make this process the active writer to the pipe
    active_pipe -> writer_exists = 1;

    pipe_check_if_write_needs_sleep(active_pipe);

    int bytes_written = 0;

    for(int i = 0; i < size; i++){
      //write the character to the
      *active_pipe -> write_cursor = buf[i];
      bytes_written = bytes_written + 1;

      //if at the end of the buffer set write cursor back to 0
      if(active_pipe -> write_cursor == active_pipe -> data + sizeof(struct pipe) + MAX_CURSOR_VALUE){
        active_pipe -> write_cursor = active_pipe -> data + sizeof(struct pipe);

      //otherwise just increment by 1
       }else{
        active_pipe ->  write_cursor = active_pipe -> write_cursor + 1;
      }
      //don't sleep if the write is done
      if(i == size - 1){
        break;
      }else{
        //check if needs to sleep
        pipe_check_if_write_needs_sleep(active_pipe);
      }
    }

    //let other processes become the writer
    //since we are done
    active_pipe -> writer_exists = 0;

    //wakeup any readers sleeping on the pipe 
    wakeup(active_pipe -> data);

    release(&global_ftable_lock);

    cprintf("bytes written = %d \n", bytes_written);

    return bytes_written;

  }

}


/*
 * arg0: int [file descriptor]
 *
 * closes the passed in file descriptor
 * returns 0 on successful close, -1 otherwise
 *
 * Error conditions:
 * arg0 is not an open file descriptor
 */

int sys_close(void) {

  //no files have even been opened so must be error
  if(first_file_allocated == 0){
    return -1;
  }

  //for getting the file descriptor
  int fd;

  acquire(&global_ftable_lock);


  //check valid_fd
  int valid_fd = argfd(0, &fd);

  if(valid_fd < 0){

    release(&global_ftable_lock);

    return -1;
  }


  //get the current number of in mem references to the file
  int num_refs = myproc() -> proc_ptr_to_global_table[fd] -> in_mem_ref_count ;

  //if there's more than one reference just decrement the
  //reference count and remove from process table
  //only by setting null pointer at fd location
  if(num_refs > 1){
    myproc() -> proc_ptr_to_global_table[fd] -> in_mem_ref_count = myproc() -> proc_ptr_to_global_table[fd] -> in_mem_ref_count - 1;

    //if dealing with a pipe need to modify it as well
    if(myproc() -> proc_ptr_to_global_table[fd] -> is_pipe == 1){
      struct pipe* pipe_to_edit = ((struct pipe*)(myproc() -> proc_ptr_to_global_table[fd] -> ref_pipe));
      //check if it's a writer or reader to the pipe
      if(myproc() -> proc_ptr_to_global_table[fd] -> permissions == O_RDONLY){
        pipe_to_edit -> ref_count_readers = pipe_to_edit -> ref_count_readers - 1;
      }else{
        pipe_to_edit -> ref_count_writers = pipe_to_edit -> ref_count_writers - 1;
      }

      //if there are no remaining readers or writers on the pipe then the allocated
      //memory must be freed
      if(pipe_to_edit -> ref_count_readers == 0 && pipe_to_edit -> ref_count_writers == 0){
        kfree((char*)pipe_to_edit -> data);
      }
    }
//else{
  //    myproc() -> proc_ptr_to_global_table[fd] -> ref_inode -> ref--;
   // }
    myproc() -> proc_ptr_to_global_table[fd] = NULL;

    release(&global_ftable_lock);

    return 0;
  //if this is the last reference then remove in the global table
  //and NULL out the process table at the fd location
}else{
    myproc() -> proc_ptr_to_global_table[fd] -> in_mem_ref_count = 0;

    //if dealing with a pipe nee to modify it as well
    if(myproc() -> proc_ptr_to_global_table[fd] -> is_pipe == 1){
      struct pipe* pipe_to_edit = ((struct pipe*)(myproc() -> proc_ptr_to_global_table[fd] -> ref_pipe));
      //check if it's a writer or reader to the pipe
      if(myproc() -> proc_ptr_to_global_table[fd] -> permissions == O_RDONLY){
        pipe_to_edit -> ref_count_readers = pipe_to_edit -> ref_count_readers - 1;
      }else{
        pipe_to_edit -> ref_count_writers = pipe_to_edit -> ref_count_writers - 1;
      }
      //if there are no remaining readers or writers on the pipe then the allocate
      //memory must be freed
      if(pipe_to_edit -> ref_count_readers == 0 && pipe_to_edit -> ref_count_writers == 0){
        kfree((char*)pipe_to_edit -> data);
      }
    }

   // myproc() -> proc_ptr_to_global_table[fd] -> ref_inode = NULL;
    myproc() -> proc_ptr_to_global_table[fd] = NULL;

    release(&global_ftable_lock);

    return 0;
  }

  release(&global_ftable_lock);


  //shouldn't get here
  return -1;
}

/*
 * arg0: int [file descriptor]
 * arg1: struct stat *
 *
 * populates the struct stat pointer passed in to the function
 *
 * returns 0 on success, -1 otherwise
 * Error conditions:
 * if arg0 is not a valid file descriptor
 * if any address within the range [arg1, arg1+sizeof(struct stat)] is invalid
 */

int sys_fstat(void) {

  //no files have even been opened so must be error
  if(first_file_allocated == 0){
    return -1;
  }

  //for getting file descriptor arg
  int fd;

  acquire(&global_ftable_lock);


  //check valid_fd
  int valid_fd = argfd(0, &fd);

  if(valid_fd < 0){

    release(&global_ftable_lock);
    return -1;
  }

  //since a stat struct is going to be populated that's how large the allocated array needs to be
  int size = sizeof(struct stat);


  //since argptr needs char* ptr use this
  //to allocate memory which will later
  //become a ptr of type struct stat*
  char* char_stat_buf;

  int error_code_argptr = argptr(1, &char_stat_buf, size);

  if(error_code_argptr < 0){

    release(&global_ftable_lock);

    return -1;
  }

  //cast to the allocated char* to struct stat*
  struct stat* stat_struct_to_fill = (struct stat*) char_stat_buf;

  //get the inode corresponding to fd
  struct inode* fd_inode = myproc() -> proc_ptr_to_global_table[fd] -> ref_inode;

  //if for some reason the inode isn't valid
  //this should yield error
  if(fd_inode == NULL){

    release(&global_ftable_lock);

    return -1;
  }


  release(&global_ftable_lock);


  //fill the stats to the struct
  concurrent_stati(fd_inode, stat_struct_to_fill);


  return 0;


}

/*
 * arg0: char * [path to the file]
 * arg1: int [mode for opening the file (see inc/fcntl.h)]
 *
 * Given a pathname for a file, sys_open() returns a file descriptor, a small,
 * nonnegative integer for use in subsequent system calls. The file descriptor
 * returned by a successful call will be the lowest-numbered file descriptor
 * not currently open for the process.
 *
 * Each open file maintains a current position, initially zero.
 *
 * returns -1 on error
 *
 * Errors:
 * arg0 points to an invalid or unmapped address
 * there is an invalid address before the end of the string
 * the file does not exist
 * since the file system is read only, flag O_CREATE is not permitted
 * there is no available file descriptor
 *
 * note that for lab1, the file system does not support file create
 */

int sys_open(void) {





  //path to file
  char *path;

  //permission
  int permission;

  //check the path is retrieved correctly
  int err_code_argstr = argstr(0, &path);

  //check if couldn't fetch the string correctly due to some error
  if(err_code_argstr < 0){

   unlockicache();
   return -1;
  }

  //check permission retrieved correctly
  int err_code_argint = argint(1, &permission);

  if(err_code_argint < 0){
    return -1;
  }

  //get the inode relevant to the path
  //and check it's not NULL
  struct inode* path_inode = namei(path);

  int permission_contains_create = 0;

  if(permission > 2){
    permission_contains_create = 1;
  }

  //remove the high bits of create if it's there
  permission = permission & 7;

  //if inode is null and create is set
  if(permission_contains_create && path_inode == NULL){

    path_inode = create_file_on_disk(path);

  }

  //if inode is NULL and flag didn't involve create that's an error
  //or create failed
  if(path_inode == NULL){
    return -1;
  }

  //need to get stats to check if console (that is Device file)
  //since a stat struct is going to be populated that's how large the allocated array needs to be
  int size = sizeof(struct stat);




  //cast to the allocated char* to struct stat*
  struct stat stat_struct_to_fill_original;

  struct stat* stat_struct_to_fill = &stat_struct_to_fill_original;


  if(stat_struct_to_fill != NULL){
    assertm(stat_struct_to_fill != NULL, "NULL POINTER");

    //fill the stats to the struct
    concurrent_stati(path_inode, stat_struct_to_fill);

  }

  if(( permission == O_WRONLY || permission == O_RDWR) && stat_struct_to_fill == NULL){

    return -1;
  }

  //if this is the first open
  //initialize the ftable lock
  if(first_file_allocated == 0){
      initlock(&global_ftable_lock, "ftable");
  }


  //create the file_info struct to save
  struct file_info fi = { .in_mem_ref_count = 1, .ref_inode = path_inode, .current_offset = 0, .permissions = permission, .is_pipe = 0, .ref_pipe = NULL };
  acquire(&global_ftable_lock);


  //if no files have been allocated to the global table
  //then it contains junk which isn't good because it could
  //look like valid data, so take this as a chance to zero it
  //out so there's no confusion
  if(first_file_allocated == 0){
    first_file_allocated = 1;
    for(int i = 0; i < NFILE; i++){
      //make all reference counts 0
      //this will be enough to know
      //if it's allocated or not
      global_file_info_array[i].in_mem_ref_count = 0;
    }
    //also NULL out the process table since it must
    //be the first allocation to the process table
    //as well
    for(int i = 0; i < NOFILE; i++){
      //set all pointers to NULL (NULL) initially that way
      //it's easy to see what the next available
      //file descriptor is since NULL will never naturally
      //appear
      myproc() -> proc_ptr_to_global_table[i] = NULL;
    }


    //since this is the first gloabl allocation it is also
    //the first local (per proccess allocation) so it's safe
    //to set the pointer to element 1 (index 0) of the global array
    //and also return fd = 0
    global_file_info_array[0] = fi;
    myproc() -> proc_ptr_to_global_table[0] = &global_file_info_array[0];

    release(&global_ftable_lock);
    return 0;
  }else{
    //in the case that this isn't the first allocation to
    //the global table no zeroing out is necessary
    //simply add to the global table



    //index to the global array which will eiher
    //find an open index in the global array or remain as
    //NFILE which would indicate global table is full
    int global_arr_index = NFILE;


    //find the first open entry in the global table to
    //create the new file struct
    for(int i = 0; i < NFILE; i++){

      struct file_info current_fi = global_file_info_array[i];

      if(current_fi.in_mem_ref_count == 0 ){
        global_arr_index = i;
        //set the ref count to 1
        global_file_info_array[i] = fi;
        break;
      }
    }


    //the global_arr_index would remains
    //as NFILE then the global array was full
    if(global_arr_index == NFILE){
      
      release(&global_ftable_lock);
      return -1;
    }

    //set the pointer in the process table
    for(int i = 0; i < NOFILE; i++){

      if(myproc() -> proc_ptr_to_global_table[i] == NULL ){
        myproc() -> proc_ptr_to_global_table[i] = &global_file_info_array[global_arr_index];


        release(&global_ftable_lock);
        //this i value is the lowest available file descriptor so return it
        return i;
      }
    }


    }


  release(&global_ftable_lock);
  //if it made it here then it didn't
  //find a valid file descriptor to return
  return -1;
}

int sys_exec(void) {


  //path to file
  char* path = (void*)0;

  //check the path is retrieved correctly
  int err_code_argstr = argstr(0, &path);
  if(err_code_argstr < 0){
     return -1;
  }


  //for the address to the first of the string args
  int addr;

  //check the address was retrieved properly
  int err_code_argint = argint(1, &addr);

  if(err_code_argint < 0){
    return -1;
  }

  //args for the new program
  char** args = (char**)addr;

//  return exec(path, args, addr);

  char* mybuf;

  //check the addr for args is valid
  //before reading in the args
  if(fetchstr(addr, &mybuf) < 0){
    return -1;
  }


  //to store how many string args exist
  int number_args = 0;


  //calculate number of args with error check
  for(int i = 0; i < 5; i++){


    if(args[i] == 0x0){
      break;
    }

    int length = fetchstr(args[i], &mybuf);

    if(length < 0){
      return -1;
    }else{
      if(mybuf == (char*)0x0){
        break;
      }else{
        number_args = number_args + 1;
      }
    }

  }


  //create vars to hold new data for registers
  uint64_t rip;
  uint64_t rdi;
  uint64_t rsi;
  uint64_t rsp;




  //get a reference to the current vspace which will
  //need to be freed later
  struct vspace vspace_to_free = myproc() -> vspace;


  //DEBUG
 // vspacedumpcode(&myproc() -> vspace);


  //create empty vspace struct to switch to
  struct vspace new_vspace;
 
  //initialize the virtual address space
  int check_vspace_init = vspaceinit(&new_vspace);

  if(check_vspace_init < 0 ){
   // vspacefree(&new_vspace);
    return -1;
  }

  int check_load_code = vspaceloadcode(&new_vspace, path, &rip);

  //failed to load the code
  if(check_load_code == 0){
    vspacefree(&new_vspace);
    return -1;
  }

  //initialize the stack, SZ_2G is a defined constant for 2G address
  //which is what user memory is capped at
  int check_init_stack = vspaceinitstack(&new_vspace, SZ_2G);


  if(check_init_stack < 0){
    vspacefree(&new_vspace);
    return -1;
  }

  //global variable to track how many pages the stack has
  //useful to limit stack pages to 10
  myproc() -> stack_page_count = 1;


  //for storing the stack addresses of the args written to stack
  char* stack_addresses[number_args];

  //can't write directly to base so
  //jump down a little bit
  int current_write_address = SZ_2G;

  int write_success = 0;


  //a null pointer for writing 0 to the stack
  char* null_pointer = NULL;


  //write each string args to the stack
  for(int i = number_args-1; i > -1; i--){

    if(args[i] == 0x0){
      break;
    }

    int length = fetchstr(args[i], &mybuf);

    if(length < 0){
      vspacefree(&new_vspace);
      return -1;
    }


    int mod8 = ((length+1)/8) + 1;

    //subtract length for each char in the string, subtract 1 more for 
    //the null terminator char
    current_write_address = current_write_address - mod8*sizeof(char*);

    write_success = vspacewritetova(&new_vspace, current_write_address, mybuf, length+1);

    if(write_success < 0){
      vspacefree(&new_vspace);
      return -1;
    }

    stack_addresses[(number_args - 1) -  i] = (char*)current_write_address;

    if(i == (number_args - 1) && number_args != 1 ){
     //write a zero to show the end of the first argument written
     current_write_address = current_write_address - sizeof(char*);

      write_success = vspacewritetova(&new_vspace, current_write_address, ((char*)&null_pointer), sizeof(char*));

      if(write_success < 0){
        vspacefree(&new_vspace);
        return -1;
      }
    }

  }




  //write a zero to show the end of args have been written to stack
  current_write_address = current_write_address - sizeof(char*);

  write_success = vspacewritetova(&new_vspace, current_write_address, ((char*)&null_pointer), sizeof(char*));

  if(write_success < 0){
    vspacefree(&new_vspace);
    return -1;
  }


  //write the addresses of the arguments as "argv" to the stack
  for(int i = 0; i < number_args; i++){

    current_write_address = current_write_address - sizeof(char*);
    write_success = vspacewritetova(&new_vspace, current_write_address, ((char*)&stack_addresses[i]), sizeof(char*));

    if(write_success < 0){
      vspacefree(&new_vspace);
      return -1;
    }

  }



  //write a final dummy address
  current_write_address = current_write_address - sizeof(char*);

  write_success = vspacewritetova(&new_vspace, current_write_address, ((char*)&null_pointer), sizeof(char*));


  if(write_success < 0){
    vspacefree(&new_vspace);
    return -1;
  }

   myproc() -> vspace = new_vspace;

  //update the registers

  //the new instruction from vspaceload code for rip
  update_rip(rip);

  //put the number of args into %rdi
  rdi = (uint64_t)number_args;
  update_rdi(rdi);


  //put the address of argv array into %rsi
  rsi = (uint64_t)current_write_address + sizeof(char*);
  update_rsi(rsi);

  //set stack pointer to lowest address of stack
  rsp = (uint64_t)current_write_address;
  update_rsp(rsp);




  //install the new vspace
  vspaceinstall(myproc());


//  struct vpage_info* myinfo = va2vpage_info(&myproc() -> vspace.regions[1], 8192);

  //DEGUB
 // vspacedumpstack(&myproc() -> vspace);
//  vspacedumpcode(&myproc() -> vspace);

  //free the old vspace
  vspacefree(&vspace_to_free);


  return 0;
}

int sys_pipe(void) {
  // LAB2

  //see if the memory can even be allocated to begin with
  char* kernel_buff = kalloc();

  //return error if can't allocate memory
  if(kernel_buff == 0){
    return -1;
  }

  //get the return array
  char* return_arr_char;

  int err_code_argptr = argptr(0, &return_arr_char, 2*sizeof(int));

  if(err_code_argptr < 0){
    return -1;
  }

  int* return_arr = (int*) return_arr_char;


  //create the pipe TODO check what fields were added in defs.h
  struct pipe new_pipe = {.data = kernel_buff, .ref_count_readers = 1, .ref_count_writers = 1, .read_cursor = kernel_buff + sizeof(struct pipe), .write_cursor = kernel_buff + sizeof(struct pipe), .writer_exists = 0, .reader_exists = 0 };

  memmove(kernel_buff, &new_pipe, sizeof(struct pipe));

 


  //create two new file_info structs that point to the buffer
  //one needs to be read only the other write only

  struct file_info fi_read_pipe = { .in_mem_ref_count = 1, .ref_inode = NULL, .current_offset = 0, .permissions = O_RDONLY, .is_pipe = 1, .ref_pipe = kernel_buff };
  struct file_info fi_write_pipe = { .in_mem_ref_count = 1, .ref_inode = NULL, .current_offset = 0, .permissions = O_WRONLY, .is_pipe = 1, .ref_pipe = kernel_buff };



 // char** pointer = &kernel_buff;

  //struct pipe* test_pipe = (struct pipe*)(&((*pointer)[0]));



  //track how many of the two file_info structs get
  //allocated, if both don't get allocated that is an error
  int num_allocated_global = 0;

  int global_index1 = -1;
  int global_index2 = -1;;

  //acquire lock since accessing shared data structure
  acquire(&global_ftable_lock);
  //allocate the file info structs
  for(int i = 0; i < NFILE; i++){

      struct file_info current_fi = global_file_info_array[i];

      if(current_fi.in_mem_ref_count == 0 && num_allocated_global == 0 ){
        global_file_info_array[i] = fi_read_pipe;
        num_allocated_global = 1;
        global_index1 = i;
      }else if(current_fi.in_mem_ref_count == 0 && num_allocated_global == 1){
        global_file_info_array[i] = fi_write_pipe;
        num_allocated_global = 2;
        global_index2 = i;
        break;
      }
    }

  //if there wasn't enough slots in the global table for both
  //file info structs return an error
  if(num_allocated_global < 2){
    release(&global_ftable_lock);
    return -1;
  }


  int num_allocated_local = 0;

  int fd1 = -1;
  int fd2 = -1;

  //if the allocation worked add to the per process table
  //set the pointer in the process table
    for(int i = 0; i < NOFILE; i++){

      if(myproc() -> proc_ptr_to_global_table[i] == NULL && num_allocated_local == 0 ){
        myproc() -> proc_ptr_to_global_table[i] = &global_file_info_array[global_index1];
        num_allocated_local = 1;
        fd1 = i;
      }else if(myproc() -> proc_ptr_to_global_table[i] == NULL && num_allocated_local == 1){
        myproc() ->  proc_ptr_to_global_table[i] = &global_file_info_array[global_index2];
        num_allocated_local = 2;
        fd2 = i;
        break;
      }
    }

  //there wasn't enough room in the per process table to allocate
  if(num_allocated_local < 2){
    release(&global_ftable_lock);
    return -1;
  }


  //pass the file descriptors to the array
  return_arr[0] = fd1;
  return_arr[1] = fd2;

  release(&global_ftable_lock);

  return 0;

}

int sys_unlink(void) {
  // LAB 4

  //path to file
  char *path;

  //check the path is retrieved correctly
  int err_code_argstr = argstr(0, &path);

  //check if couldn't fetch the string correctly due to some error
  if(err_code_argstr < 0){
   return -1;
  }


  struct inode* path_inode = namei(path);

  path_inode -> ref--;

  //no such file exists
  if(path_inode == NULL){

    return -1;

  }
  //synchronize the inode
  path_inode -> valid = 0;
  locki(path_inode);
  unlocki(path_inode);

  //don't allow deleting directory
  if(path_inode -> type == T_DIR ){
    return -1;
  }

  //if another process has a reference
  //that's an error
 // if(path_inode -> ref > 0){
 //   return -1;
  //}

  int safe_to_delete = 1;


  acquire(&global_ftable_lock);
  for(int i = 0; i < NFILE; i++){
    struct inode* current_inode = global_file_info_array[i].ref_inode;

    if(current_inode -> inum == path_inode -> inum && global_file_info_array[i].in_mem_ref_count > 0){
      safe_to_delete = 0;
    }

  }
  release(&global_ftable_lock);

  if(safe_to_delete == 0){
    return -1;
  }

  delete_file(path_inode -> inum);

  return 0;
}

//need to update in mem reference counts
//both for file_info structs as well as
//for the reader and writer counts of pipes (if any pipes are referenced) 
void update_global_table_on_fork(struct proc* child){

  acquire(&global_ftable_lock);
  for(int i = 0; i < NOFILE; i++){
    if(child -> proc_ptr_to_global_table[i] != NULL){
      child -> proc_ptr_to_global_table[i] -> in_mem_ref_count = child -> proc_ptr_to_global_table[i] -> in_mem_ref_count + 1;

      //if the file_info struct references a pipe
      if(child -> proc_ptr_to_global_table[i] -> is_pipe == 1){
        struct pipe* pipe_to_edit = ((struct pipe*)child -> proc_ptr_to_global_table[i] -> ref_pipe);

        //check if it's read or write pipe reference
        if(child -> proc_ptr_to_global_table[i] -> permissions == O_RDONLY){
          pipe_to_edit -> ref_count_readers = pipe_to_edit -> ref_count_readers + 1;
        }else{
          pipe_to_edit -> ref_count_writers = pipe_to_edit -> ref_count_writers + 1;
        }

      }
    }
  }

  release(&global_ftable_lock);



}



void close_all_fds_for_process(struct proc* exiting_proc){



  acquire(&global_ftable_lock);
  for(int i = 0; i < NOFILE; i++){
    if(exiting_proc -> proc_ptr_to_global_table[i] != NULL){

      //get the current number of in mem references to the file
      int num_refs = myproc() -> proc_ptr_to_global_table[i] -> in_mem_ref_count ;

      //if there's more than one reference just decrement the
      //reference count and remove from process table
      //only by setting null pointer at fd location
      if(num_refs > 1){
        myproc() -> proc_ptr_to_global_table[i] -> in_mem_ref_count = myproc() -> proc_ptr_to_global_table[i] -> in_mem_ref_count - 1;
        //if it's a pipe then also need to modify the global pipes count
        if(myproc() -> proc_ptr_to_global_table[i] -> is_pipe == 1){
          struct pipe* pipe_to_edit = ((struct pipe*)(myproc() -> proc_ptr_to_global_table[i] -> ref_pipe));
          //check if it's a writer or reader to the pipe
          if(myproc() -> proc_ptr_to_global_table[i] -> permissions == O_RDONLY){
            pipe_to_edit -> ref_count_readers = pipe_to_edit -> ref_count_readers - 1;
          }else{
            pipe_to_edit -> ref_count_writers = pipe_to_edit -> ref_count_writers - 1;
          }

          //if there are no remaining readers or writers on the pipe then the allocated 
          //memory must be freed
          if(pipe_to_edit -> ref_count_readers == 0 && pipe_to_edit -> ref_count_writers == 0){
            kfree((char*)pipe_to_edit -> data);
          }
        }
//else{
  //        myproc() -> proc_ptr_to_global_table[i] -> ref_inode -> ref--;
    //    }
        myproc() -> proc_ptr_to_global_table[i] = NULL;

      //if this is the last reference then remove in the global table
      //and NULL out the process table at the fd location
      }else{
        myproc() -> proc_ptr_to_global_table[i] -> in_mem_ref_count = 0;
        if(myproc() -> proc_ptr_to_global_table[i] -> is_pipe == 1){
          struct pipe* pipe_to_edit = ((struct pipe*)(myproc() -> proc_ptr_to_global_table[i] -> ref_pipe));
          //check if it's a writer or reader to the pipe
          if(myproc() -> proc_ptr_to_global_table[i] -> permissions == O_RDONLY){
            pipe_to_edit -> ref_count_readers = pipe_to_edit -> ref_count_readers - 1;
          }else{
            pipe_to_edit -> ref_count_writers = pipe_to_edit -> ref_count_writers - 1;
          }

          //if there are no remaining readers or writers on the pipe then the allocated
          //memory must be freed
          if(pipe_to_edit -> ref_count_readers == 0 && pipe_to_edit -> ref_count_writers == 0){
            kfree((char*)pipe_to_edit -> data);
          }
        }
//else{
  //        myproc() -> proc_ptr_to_global_table[i] -> ref_inode -> ref--;
    //    }

      //  myproc() -> proc_ptr_to_global_table[i] -> ref_inode = NULL;
        myproc() -> proc_ptr_to_global_table[i] = NULL;
      }
    }
  }
  release(&global_ftable_lock);




}



