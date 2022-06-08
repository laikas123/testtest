// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xk/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include <cdefs.h>
#include <defs.h>
#include <file.h>
#include <fs.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <sleeplock.h>
#include <spinlock.h>
#include <stat.h>

#include <buf.h>


#define NULL ((void *)0)

//int first_file_created_by_user = 0;
struct spinlock create_file_lock;
uint create_file_lock_in_use = 0;
int create_file_lock_initialized = -1;


uchar create_set_low_bitmask(int);
uchar create_set_high_bitmask(int);
static struct inode *iget(uint, uint);

void update_extra_extents_flag(void);


// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb;

// Read the super block.
void readsb(int dev, struct superblock *sb) {
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// range of blocks holding the file's content.
//
// The inodes themselves are contained in a file known as the
// inodefile. This allows the number of inodes to grow dynamically
// appending to the end of the inode file. The inodefile has an
// inum of 1 and starts at sb.startinode.
//
// The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The cached
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->flags.
//
// Since there is no writing to the file system there is no need
// for the callers to worry about coherence between the disk
// and the in memory copy, although that will become important
// if writing to the disk is introduced.
//
// Clients use iget() to populate an inode with valid information
// from the disk. idup() can be used to add an in memory reference
// to and inode. irelease() will decrement the in memory reference count
// and will free the inode if there are no more references to it,
// freeing up space in the cache for the inode to be used again.



struct {
  struct spinlock lock;
  struct inode inode[NINODE];
  struct inode inodefile;
} icache;

void lockicache(){

  locki(&icache.inodefile);

}

void unlockicache(){

  unlocki(&icache.inodefile);

}


// Find the inode file on the disk and load it into memory
// should only be called once, but is idempotent.
static void init_inodefile(int dev) {
  struct buf *b;
  struct dinode di;

  b = bread(dev, sb.inodestart);
  memmove(&di, b->data, sizeof(struct dinode));

  icache.inodefile.inum = INODEFILEINO;
  icache.inodefile.dev = dev;
  icache.inodefile.type = di.type;
  icache.inodefile.valid = 1;
  icache.inodefile.ref = 1;

  icache.inodefile.devid = di.devid;
  icache.inodefile.size = di.size;
  icache.inodefile.data = di.data;

  brelse(b);
}

void iinit(int dev) {
  int i;

  initlock(&icache.lock, "icache");
  for (i = 0; i < NINODE; i++) {
    initsleeplock(&icache.inode[i].lock, "inode");
  }
  initsleeplock(&icache.inodefile.lock, "inodefile");

  readsb(dev, &sb);
  cprintf("sb: size %d nblocks %d bmap start %d inodestart %d\n", sb.size,
          sb.nblocks, sb.bmapstart, sb.inodestart);

  init_inodefile(dev);
}





//uses bitmap to see if block is 
//free or used
int get_block_state(int blk_num){


  int block_steps = (blk_num - sb.bmapstart)/4096;

  int block_offset = blk_num - block_steps * 4096;

  int buf_data_steps = block_offset/8;

  int byte_offset = block_offset - buf_data_steps*8;

  struct buf* bmap_buf;

  bmap_buf = bread(1, sb.bmapstart + block_steps);


  uchar my_bitmask = create_set_high_bitmask(byte_offset);

  uchar new_num = my_bitmask & bmap_buf->data[buf_data_steps];

  new_num = (new_num << (7 - byte_offset)) >> 7;


  brelse(bmap_buf);

  return new_num;

}



struct inode* create_file_on_disk(char* filename){


  //checking if lock needs initialized must be atomic
  pushcli();

  int val = create_file_lock_initialized;

  if(val == -1){
    create_file_lock_initialized = 1;
  }

  popcli();

  if(val == -1){
    initlock(&create_file_lock, "create");
  }

  acquire(&create_file_lock);

  if(create_file_lock_in_use == 0){
    create_file_lock_in_use = 1;
  }else{
    while(create_file_lock_in_use == 1){
      sleep(9999, &create_file_lock);
    }
    struct inode* check_created = namei(filename);

    //the file was already created while asleep
    if(check_created != NULL){
      wakeup(9999);
      release(&create_file_lock);
      return check_created;
    }

    create_file_lock_in_use = 1;
  }

  release(&create_file_lock);


  //this let's me know if I need to increase root dir size or not
  int file_used_deleted_files_space = 0;

  //need to see the last block used
  //by the previous dinode, so the caller
  //knows what block to use for the start of this file
  int last_free_block = -1;

  int found_free_inode = 0;

  struct buf* last_blk;

  //in looking at the code it's already obvious the first dinodes are found
  //in the last block of the inode file
  int last_block_num = 33;

  last_blk = bread(icache.inodefile.dev, last_block_num);

  struct dinode* last_block_dinodes = (struct dinode*)(last_blk -> data);



  //since this is the 7th block 6*4 = 24
  //so the first inode in this block has 
  //inum = 24
  int inum = 24;

  // //dinode = 128 bytes, block = 512 bytes 
  // //so 4 dinodes per block
  // for(int i = 0; i < 4; i++){
  //   if(last_block_dinodes[i].type == 0){
  //     inum = inum + i;
  //     found_free_inode = 1;
  //     last_block_dinodes[i].type = 2;
  //     last_block_dinodes[i].size = 0;

  //     int num_data_blocks = 20;

  //     //signifies this was a space of a recently deleted file, so can potentially use more than 20 (default blocks)
  //     if(last_block_dinodes[i].extra_extents_blk_nums[25] != 0 && last_block_dinodes[i].extra_extents_blk_nums[24] != 0 ){
  //       num_data_blocks = (last_block_dinodes[i].extra_extents_blk_nums[25]-last_block_dinodes[i].extra_extents_blk_nums[24])+1;
  //       file_used_deleted_files_space = 1;
  //       if(last_block_dinodes[i].extra_extents_blk_nums[24] != last_free_block+1){
  //         cprintf("inum = %d and filename = %s \n", inum, filename);
  //         panic("error or first time extra extents are used");
  //       }
  //     }

  //     struct extent data;
  //     data.startblkno = last_free_block+1;
  //     data.nblocks = num_data_blocks;
  //     last_block_dinodes[i].data = data;
  //     //update the bimap that these blocks are in use
  //     for(int j = 0; j < num_data_blocks; j++){
  //       set_blk_num_in_bitmap(data.startblkno + j, 1);
  //      // if(j != 0){
  //       //  last_block_dinodes[i].extra_extents_blk_nums[j - 1] = data.startblkno + j;
  //      // }
  //     }
  //     //flag to let sysfile know that extra extents array isn't yet used
  //     last_block_dinodes[i].extra_extents_blk_nums[27] = -1;
  //     //mark number of contiguous blocks
  //     last_block_dinodes[i].extra_extents_blk_nums[26] = last_block_dinodes[i].data.nblocks;
  //     break;
  //   }else{
  //     last_free_block = last_block_dinodes[i].data.startblkno + last_block_dinodes[i].data.nblocks - 1;
  //   }
  // }

  // if(found_free_inode == 1 && last_free_block != -1){
  //   bwrite(last_blk);
  //   brelse(last_blk);
  // }



 //TODO to support also checking earlier blocks (e.g. if earlier inums get deleted)
 //here is where I can do that without deleting all the stuff above this...
  if(found_free_inode == 0 || last_free_block == -1){
    brelse(last_blk);

    int check_blk_num = 34;

    inum = 28;

    while(found_free_inode == 0){

      struct buf* last_blk = bread(1, check_blk_num);

      struct dinode* last_block_dinodes = (struct dinode*)(last_blk -> data);

      for(int i = 0; i < 4; i++){
        if(last_block_dinodes[i].type == 0){
          inum = inum + i;
          found_free_inode = 1;
          last_block_dinodes[i].type = 2;
          last_block_dinodes[i].size = 0;

          int num_data_blocks = 20;

          //signifies this was a space of a recently deleted file, so can potentially use more than 20 (default blocks)
          if(last_block_dinodes[i].extra_extents_blk_nums[25] != 0 && last_block_dinodes[i].extra_extents_blk_nums[24] != 0 ){
            num_data_blocks = (last_block_dinodes[i].extra_extents_blk_nums[25]-last_block_dinodes[i].extra_extents_blk_nums[24])+1;
            file_used_deleted_files_space = 1;
            if(inum == 28){
              last_free_block = 48;
              num_data_blocks = 20;
              file_used_deleted_files_space = 0;
            }else if(last_block_dinodes[i].extra_extents_blk_nums[24] != last_free_block+1){
              cprintf("inum = %d and filename = %s \n and last free block = %d \n and start = %d and end = %d", inum, filename, last_free_block, last_block_dinodes[i].extra_extents_blk_nums[24], last_block_dinodes[i].extra_extents_blk_nums[25]);
              panic("error or first time extra extents are used");
            }
          }

          struct extent data;
          data.startblkno = last_free_block+1;
          if(inum == 28){
            cprintf("last free block = %d \n", last_free_block+1);
          }
          data.nblocks = num_data_blocks;
          last_block_dinodes[i].data = data;
          //update the bimap that these blocks are in use
          for(int j = 0; j < num_data_blocks; j++){
            set_blk_num_in_bitmap(data.startblkno + j, 1);
          }
          //flag to let sysfile know that extra extents array isn't yet used
          last_block_dinodes[i].extra_extents_blk_nums[27] = -1;
          //mark number of contiguous blocks
          last_block_dinodes[i].extra_extents_blk_nums[26] = last_block_dinodes[i].data.nblocks;
          break;
        }else{
          last_free_block = last_block_dinodes[i].data.startblkno + last_block_dinodes[i].data.nblocks - 1;
        }
      }


      if(found_free_inode == 1 && last_free_block != -1){ 
        if(inum == 28){
          cprintf("yes wrote the blk back \n");
        }
        bwrite(last_blk);
        brelse(last_blk);
        last_blk = bread(1, check_blk_num);
        

        last_block_dinodes = (struct dinode*)(last_blk -> data);
        if(inum == 28){
          cprintf("check blk num = %d \n", check_blk_num);
          cprintf("type retrieved = %d size = %d\n", last_block_dinodes[0].type, last_block_dinodes[0].size, last_block_dinodes[0].type);

        }
        break;
      }else{
        check_blk_num++;
        //since 4 dinodes per block
        inum = inum + 4;
        brelse(last_blk);
        if(check_blk_num > 36){
          break;
        }
      }

    }

    if(found_free_inode == 0){
      cprintf("found free inode = %d and last_free_block = %d \n", found_free_inode, last_free_block);
      panic("couldn't find free inode or couldn't find last free block");
    }

  }



  //need to update the inode file to have a larger size since
  //another file was appended
  struct buf* inodefileblock = bread(1, 27);

  struct dinode* inodefileextents = (struct dinode*)inodefileblock -> data;

  //increase the size of the inode file
//  inodefileextents[0].size = inodefileextents[0].size + sizeof(struct dinode);   <--- this doesn't actually need to happen, the size is set to maximum in mkfs.c



  //only increaes size if it didn't reuse space of previously deleted file
  if(file_used_deleted_files_space == 0){
    //increase the size of the root directory file since it will soon 
    //have another entry
    inodefileextents[1].size = inodefileextents[1].size + sizeof(struct dirent);
  }

  bwrite(inodefileblock);
  brelse(inodefileblock);

  icache.inodefile.valid = 0;

  //synchronize the inode for icacheinodefile
  locki(&icache.inodefile);
  unlocki(&icache.inodefile);


  //synchronize the newly modified dinode
  //with the inode
//  struct inode* syncnew = iget(1, inum);
 // syncnew -> valid = 0;
 // locki(syncnew);
 // unlocki(syncnew);


  //add the new inode to the root directory
  struct dirent new_dirent;

  strncpy(new_dirent.name, filename, DIRSIZ);
  new_dirent.inum = inum;


  int root_block_to_get = 37 + (inum*sizeof(struct dirent))/BSIZE;

  int root_block_offset = inum%(BSIZE/sizeof(struct dirent));

  struct buf* root_extents = bread(1, root_block_to_get);

  struct dirent* root_extents_files = (struct dirent*)(root_extents -> data);

  root_extents_files[root_block_offset] = new_dirent;

  bwrite(root_extents);
  brelse(root_extents);

  //synchronize the root dir inode
  struct inode* syncroot = iget(1, 1);
  syncroot -> valid = 0;
  locki(syncroot);
  unlocki(syncroot);

  struct inode* return_inode = iget(1, inum);
  return_inode -> valid = 0;
  locki(return_inode);
  unlocki(return_inode);

  acquire(&create_file_lock);
  create_file_lock_in_use = 0;
  wakeup(9999);
  release(&create_file_lock);

  return return_inode;

}





void delete_file(int inum){


  struct dirent null_dirent;

  memset(&null_dirent, 0, sizeof(struct dirent));

  struct buf* root_extents = bread(1, 37);

  struct dirent* root_extents_files = (struct dirent*)(root_extents -> data);


  root_extents_files[inum] = null_dirent;

  bwrite(root_extents);
  brelse(root_extents);


  //zero out the corresponding inode portion of the
  //inode file
  int block_num = 27+inum/4;

  int block_offset = inum % 4;

  struct buf* inode_blk = bread(1, block_num);

  struct dinode* dinode_list = (struct dinode*)(inode_blk -> data);

  int start_block = dinode_list[block_offset].data.startblkno;
  int end_block = start_block + dinode_list[block_offset].data.nblocks - 1;

  for(int i = start_block; i < end_block + 1; i++){

   // cprintf("block %d has value %d \n", i, get_block_state(i));
    set_blk_num_in_bitmap(i, 0);
   // cprintf("block %d has value %d \n", i, get_block_state(i));

  }

  memset(&dinode_list[block_offset], 0, sizeof(struct dinode));


  //small trick to tell files the amount of contiguous blocks
  //available from a deleted file, for now just use all blocks
  //that a deleted file frees up
  dinode_list[block_offset].extra_extents_blk_nums[25] = end_block;
  dinode_list[block_offset].extra_extents_blk_nums[24] = start_block;


  bwrite(inode_blk);
  brelse(inode_blk);

  icache.inodefile.valid = 0;
  //synchronize the inode for icacheinodefile
  locki(&icache.inodefile);
  unlocki(&icache.inodefile);


  struct inode* syncroot = iget(1, 1);
  syncroot -> valid = 0;
  locki(syncroot);
  unlocki(syncroot);

}



void set_blk_num_in_bitmap(int blk_num, int val){

  //since it's a bit can only set to 0 or 1...
  if(val != 1 && val != 0){
    panic("can't set bitmap bit to anything besides 1 or 0");
  }

  int block_steps = (blk_num - sb.bmapstart)/4096;

  int block_offset = blk_num - block_steps * 4096;

  int buf_data_steps = block_offset/8;

  int byte_offset = block_offset - buf_data_steps*8;

  struct buf* bmap_buf;

  bmap_buf = bread(1, sb.bmapstart + block_steps);

  //if setting the bit low
  if(val == 0){
    bmap_buf -> data[buf_data_steps] = bmap_buf->data[buf_data_steps] & create_set_low_bitmask(byte_offset);
  //if setting the bit high
  }else{
    bmap_buf -> data[buf_data_steps] = bmap_buf->data[buf_data_steps] | create_set_high_bitmask(byte_offset);
  }

  bwrite(bmap_buf);
  brelse(bmap_buf);


}

int number_of_high_bits(uchar bmap_byte){

  int number_bits_high = 0;

  for(int i = 0; i < 8; i++){

    uchar my_bitmask = create_set_high_bitmask(i);

    uchar new_num = my_bitmask & bmap_byte;

    new_num = (new_num << (7 - i)) >> 7;

    if(new_num == (uchar)1){
      number_bits_high++;
    }

  }

  return number_bits_high;


}




//struct buf get_next_free_block_and_mark


//create a bitmask that when "AND" is applied to a byte
//will set the bit at "bit_position" low 0 is the rightmost bit
//7 is the left most bit
uchar create_set_low_bitmask(int bit_position){

  uchar amt_to_add = 1;

  uchar amt = 0;

  for(int i = 0; i < 8; i++){

    if(i == 0){
      amt_to_add = 1;
    }else{
      amt_to_add = amt_to_add * 2;
    }
    if(i != bit_position){
      amt += amt_to_add;
    }
  }

  return amt;
}

//create a bitmask that when "OR" is applied to a byte
//will set the bit at "bit_position" high, 0 is the rightmost bit
//7 is the left most bit
uchar create_set_high_bitmask(int bit_position){

  uchar amt_to_add = 1;

  uchar amt = 0;

  for(int i = 0; i < 8; i++){

    if(i == 0){
      amt_to_add = 1;
    }else{
      amt_to_add = amt_to_add * 2;
    }
    if(i == bit_position){
      amt += amt_to_add;
    }
  }

  return amt;
}




// Reads the dinode with the passed inum from the inode file.
// Threadsafe, will acquire sleeplock on inodefile inode if not held.
void read_dinode(uint inum, struct dinode *dip) {
  int holding_inodefile_lock = holdingsleep(&icache.inodefile.lock);
  if (!holding_inodefile_lock)
    locki(&icache.inodefile);

  int value = readi(&icache.inodefile, (char *)dip, INODEOFF(inum), sizeof(*dip));

  if(inum == 28){
    cprintf("readi value = %d \n", value);
  }
  if (!holding_inodefile_lock)
    unlocki(&icache.inodefile);

}

void write_dinode(uint inum, struct dinode dip) {
  int holding_inodefile_lock = holdingsleep(&icache.inodefile.lock);
  if (!holding_inodefile_lock)
    locki(&icache.inodefile);

  read_then_write_dinode(&icache.inodefile, dip, INODEOFF(inum));

  if (!holding_inodefile_lock)
    unlocki(&icache.inodefile);

}

// Read data from inode.
// Returns number of bytes read.
// Caller must hold ip->lock.
void read_then_write_dinode(struct inode *ip, struct dinode src, uint off) {
  uint tot, m;
  struct buf *bp;

  if (!holdingsleep(&ip->lock))
    panic("not holding lock");

  
  //get the block based on device and block number, note sleeplock is held
  bp = bread(ip->dev, ip->data.startblkno + off / BSIZE);

  struct dinode* to_modify = bp -> data + off %BSIZE;

  *to_modify = src;

 // to_modify -> size = n;

 // memmove(bp->data + off % BSIZE, src,  n);
  bwrite(bp);
  //release the locked block sleep lock and move to most recently used (MRU) list
  brelse(bp);
  
 // return n;
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not read
// the inode from from disk.
static struct inode *iget(uint dev, uint inum) {
  struct inode *ip, *empty;

  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for (ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++) {
    if (ip->ref > 0 && ip->dev == dev && ip->inum == inum) {
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if (empty == 0 && ip->ref == 0) // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode cache entry.
  if (empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->ref = 1;
  ip->valid = 0;
  ip->dev = dev;
  ip->inum = inum;

  release(&icache.lock);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode *idup(struct inode *ip) {
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode cache entry can
// be recycled.
void irelease(struct inode *ip) {
  acquire(&icache.lock);
  // inode has no other references release
  if (ip->ref == 1)
    ip->type = 0;
  ip->ref--;
  release(&icache.lock);
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void locki(struct inode *ip) {
  struct dinode dip;

  if(ip == 0 || ip->ref < 1)
    panic("locki");

  acquiresleep(&ip->lock);

  if (ip->valid == 0) {

    if (ip != &icache.inodefile)
      locki(&icache.inodefile);
    read_dinode(ip->inum, &dip);
    if (ip != &icache.inodefile)
      unlocki(&icache.inodefile);

    ip->type = dip.type;
    ip->devid = dip.devid;

    ip->size = dip.size;
    ip->data = dip.data;

    ip->valid = 1;

    cprintf("ip type = %d \n", dip.type);
    cprintf("extensts special = %d \n", dip.extra_extents_blk_nums[24]);


    //copy the block numbers for extra extents
    for(int i = 0; i < 28; i++){
      ip -> extra_extents_blk_nums[i] = dip.extra_extents_blk_nums[i];
    }


    if (ip->type == 0){
      cprintf("inode num is %d \n", ip -> inum);
      panic("iget: no type");
    }
  }
}

// Unlock the given inode.
void unlocki(struct inode *ip) {
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("unlocki");

  releasesleep(&ip->lock);
}

// threadsafe stati.
void concurrent_stati(struct inode *ip, struct stat *st) {
  locki(ip);
  stati(ip, st);
  unlocki(ip);
}

// Copy stat information from inode.
// Caller must hold ip->lock.
void stati(struct inode *ip, struct stat *st) {
  if (!holdingsleep(&ip->lock))
    panic("not holding lock");

  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->size = ip->size;
}

// threadsafe readi.
int concurrent_readi(struct inode *ip, char *dst, uint off, uint n) {
  int retval;

  locki(ip);
  retval = readi(ip, dst, off, n);
  unlocki(ip);

  return retval;
}

// Read data from inode.
// Returns number of bytes read.
// Caller must hold ip->lock.
int readi(struct inode *ip, char *dst, uint off, uint n) {
  uint tot, m;
  struct buf *bp;

  if (!holdingsleep(&ip->lock))
    panic("not holding lock");

  if (ip->type == T_DEV) {
    if (ip->devid < 0 || ip->devid >= NDEV || !devsw[ip->devid].read)
      return -1;
    return devsw[ip->devid].read(ip, dst, n);
  }

  if (off > ip->size || off + n < off)
    return -1;
  if (off + n > ip->size)
    n = ip->size - off;

  for (tot = 0; tot < n; tot += m, off += m, dst += m) {
    bp = bread(ip->dev, ip->data.startblkno + off / BSIZE);
   
    cprintf("blk num read = %d", ip->data.startblkno + off / BSIZE);
    
    m = min(n - tot, BSIZE - off % BSIZE);
    memmove(dst, bp->data + off % BSIZE, m);
    brelse(bp);
  }
  return n;
}

// threadsafe writei.
int concurrent_writei(struct inode *ip, char *src, uint off, uint n) {
  int retval;

  locki(ip);
  retval = writei(ip, src, off, n);
  unlocki(ip);

  return retval;
}

// Write data to inode.
// Returns number of bytes written.
// Caller must hold ip->lock.
int writei(struct inode *ip, char *src, uint off, uint n) {
  if (!holdingsleep(&ip->lock))
    panic("not holding lock");

  if (ip->type == T_DEV) {
    if (ip->devid < 0 || ip->devid >= NDEV || !devsw[ip->devid].write)
      return -1;
    return devsw[ip->devid].write(ip, src, n);
  }
  // read-only fs, writing to inode is an error
  return -1;
}

// Directories

int namecmp(const char *s, const char *t) { return strncmp(s, t, DIRSIZ); }

struct inode *rootlookup(char *name) {
  return dirlookup(namei("/"), name, 0);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode *dirlookup(struct inode *dp, char *name, uint *poff) {
  uint off, inum;
  struct dirent de;

  if (dp->type != T_DIR)
    panic("dirlookup not DIR");

  for (off = 0; off < dp->size; off += sizeof(de)) {
    if (readi(dp, (char *)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if (de.inum == 0)
      continue;
    if (namecmp(name, de.name) == 0) {
      // entry matches path element
      if (poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char *skipelem(char *path, char *name) {
  char *s;
  int len;

  while (*path == '/')
    path++;
  if (*path == 0)
    return 0;
  s = path;
  while (*path != '/' && *path != 0)
    path++;
  len = path - s;
  if (len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while (*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode *namex(char *path, int nameiparent, char *name) {
  struct inode *ip, *next;

  if (*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(namei("/"));

  while ((path = skipelem(path, name)) != 0) {
    locki(ip);
    if (ip->type != T_DIR) {
      unlocki(ip);
      goto notfound;
    }

    // Stop one level early.
    if (nameiparent && *path == '\0') {
      unlocki(ip);
      return ip;
    }

    if ((next = dirlookup(ip, name, 0)) == 0) {
      unlocki(ip);
      goto notfound;
    }

    unlocki(ip);
    irelease(ip);
    ip = next;
  }
  if (nameiparent)
    goto notfound;

  return ip;

notfound:
  irelease(ip);
  return 0;
}

struct inode *namei(char *path) {
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode *nameiparent(char *path, char *name) {
  return namex(path, 1, name);
}


void update_extra_extents_flag(void){

  struct buf* inodefileblock;
  struct dinode* dinodelist;

  int breakall = 0;

  int inum = 0;

  for(int i = 27; i < 34; i++){
    inodefileblock = bread(1, i);
    dinodelist = (struct dinode*)inodefileblock -> data;

    cprintf("i val reached = %d \n", i);

    for(int j = 0; j < 4; j++){
      if(dinodelist[j].type == 0 || (i == 33 && j == 1)){
        breakall = 1;
        break;
      }else{
        //-1 implies no extra extents used
        //and that all blocks are contiguous
        dinodelist[j].extra_extents_blk_nums[27] = -1;

        //mark how many blocks are contiguous here
        dinodelist[j].extra_extents_blk_nums[26] = dinodelist[j].data.nblocks;
      }
    }
    bwrite(inodefileblock);
    brelse(inodefileblock);
    if(breakall == 1){
      break;
    }
  }



  //zero out the remaining files dinodes (useful for later checks to free inodes)
  for(int i = 33; i < 37; i++){

    inodefileblock = bread(1, i);
    dinodelist = (struct dinode*)inodefileblock -> data;

    for(int j = 0; j < 4; j++){
        if(i == 33 && j== 0){
          continue;
        }else{
          //dinodelist[j].type = 0;
          memset(&dinodelist[j], 0, sizeof(struct dinode));
        }
    }
    bwrite(inodefileblock);
    brelse(inodefileblock);
  }



  icache.inodefile.valid = 0;
  //synchronize the inode for icacheinodefile
  locki(&icache.inodefile);
  unlocki(&icache.inodefile);




}
