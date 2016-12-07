#include "filesys/inode.h"
#include <debug.h>
#include <round.h>
#include <string.h>
#include "threads/synch.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

 //indirect_index points to one of these which points to data blocks
 //double_indirect_index points to one of these which points to 128 more of them which each point to data blocks
 struct inode_indirect_block_sector {
   block_sector_t blocks[INDIRECT_BLOCKS_PER_SECTOR];
 };

//converts an index to a sector number (index created from file pos / BLOCK_SECTOR_SIZE)
block_sector_t index_to_sector (const struct inode_disk *idisk, off_t index) {
   int index_base = 0;
   block_sector_t ret;

   if (index < index_base) return -1;
    // direct blocks
   if (index < DIRECT_BLOCKS_COUNT) {
     return idisk->direct_blocks[index];
   }
   index_base += DIRECT_BLOCKS_COUNT;
 
   // indirect block
   if (index < index_base + INDIRECT_BLOCKS_PER_SECTOR) {
    //calloc initializes it to all 0's
     struct inode_indirect_block_sector* indirect_idisk = calloc(1, sizeof(struct inode_indirect_block_sector));
     block_read (fs_device, idisk->indirect_block, indirect_idisk);
 
     ret = indirect_idisk->blocks[ index - index_base ];
     free(indirect_idisk); 
 
     return ret;
   }
   index_base += INDIRECT_BLOCKS_PER_SECTOR;
 
   // doubly indirect block
   if (index < index_base + INDIRECT_BLOCKS_PER_SECTOR * INDIRECT_BLOCKS_PER_SECTOR) {
     off_t index_first =  (index - index_base) / INDIRECT_BLOCKS_PER_SECTOR;  //how far to index into first indirect_block_sector
     off_t index_second = (index - index_base) % INDIRECT_BLOCKS_PER_SECTOR;  //how far to index into second indirect_block_sector

     struct inode_indirect_block_sector *indirect_idisk = calloc(1, sizeof(struct inode_indirect_block_sector));

     block_read (fs_device, idisk->doubly_indirect_block, indirect_idisk);
     block_read (fs_device, indirect_idisk->blocks[index_first], indirect_idisk);
     ret = indirect_idisk->blocks[index_second];
 
     free(indirect_idisk);
     return ret;
   }
   //invalid, over ~8MB big
   return -1;
 }

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);
  if (pos >= 0 && pos < inode->data.length)
    return index_to_sector(&inode->data, pos / BLOCK_SECTOR_SIZE);
  else
    return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

//recursive alloc call for indirect and double indirect blocks
bool inode_alloc_indirect(block_sector_t* block, size_t num_sectors, int level){
  static char zeros[BLOCK_SECTOR_SIZE];

  struct inode_indirect_block_sector indirect_block;
  if(level == 0){  //base level, also allocates indirect blocks
    if(*block == 0){  //same as direct block allocation
      if(!free_map_allocate(1, block)) return false;
      block_write (fs_device, *block, zeros);
    }
    return true;
  }
  if(*block == 0){  //same as direct block allocation
    free_map_allocate(1, block);
    block_write (fs_device, *block, zeros);
  }
  
  block_read(fs_device, *block, &indirect_block);   //read block into indirect_block_sector for reading

  int blocks = level == 1 ? num_sectors : DIV_ROUND_UP(num_sectors, INDIRECT_BLOCKS_PER_SECTOR);
  //if level 2, this gets the number of indirect blocks being used by the double indirect block
  int i;

  for(i = 0; i < blocks; i++){
    int subsize;
    if(level == 1){
      subsize = 1;
    } else {
       subsize = (num_sectors < INDIRECT_BLOCKS_PER_SECTOR ? num_sectors : INDIRECT_BLOCKS_PER_SECTOR);
    }
    if(!inode_alloc_indirect(&indirect_block.blocks[i], subsize, level -1))
      return false;
    num_sectors -= subsize;
  }
  //write indirect_block back to block
  block_write(fs_device, *block, &indirect_block);
  return true;


}

 //actually reserve all the blocks for the inode
bool inode_alloc(struct inode_disk* disk_inode, off_t length){
   int sectors = bytes_to_sectors(length);
   static char zeros[BLOCK_SECTOR_SIZE];   //used for zeroing out blocks
   int i;

   // alloc direct blocks
   int blocks = sectors < DIRECT_BLOCKS_COUNT ? sectors : DIRECT_BLOCKS_COUNT;
   for(i = 0; i < blocks; i++){
    if(disk_inode->direct_blocks[i] == 0){   //empty 
      if(!free_map_allocate(1, &disk_inode->direct_blocks[i])){
        return false;
      }
       block_write (fs_device, disk_inode->direct_blocks[i], zeros);
    }
   }
   if((sectors -= blocks) == 0) return true;

   //alloc indirect blocks
   blocks = sectors < INDIRECT_BLOCKS_PER_SECTOR ? sectors : INDIRECT_BLOCKS_PER_SECTOR;
   if(!inode_alloc_indirect(&disk_inode->indirect_block, blocks, 1 ))
    return false;
   if((sectors -= blocks) == 0) return true;

   //alloc doubly indirect blocks
   blocks = sectors < INDIRECT_BLOCKS_PER_SECTOR * INDIRECT_BLOCKS_PER_SECTOR
    ? sectors : INDIRECT_BLOCKS_PER_SECTOR * INDIRECT_BLOCKS_PER_SECTOR;
   if(!inode_alloc_indirect(&disk_inode->doubly_indirect_block, blocks, 2 ))
    return false;

   if((sectors -= blocks) == 0) return true;
   return false;

}

//recursive alloc call for indirect and double indirect blocks
bool inode_dealloc_indirect(block_sector_t* block, size_t num_sectors, int level){
  static char zeros[BLOCK_SECTOR_SIZE];

  struct inode_indirect_block_sector indirect_block;
  if(level == 0){  //base level
    free_map_release(block ,1);
    return true;
  }
  block_read(fs_device, *block, &indirect_block);   //read block into indirect_block_sector for reading
  
  int blocks = level == 1 ? num_sectors : DIV_ROUND_UP(num_sectors, INDIRECT_BLOCKS_PER_SECTOR);
  int i;

  for(i = 0; i < blocks; i++){
    int subsize;
    if(level == 1){
      subsize = 1;
    } else{
       subsize = (num_sectors < INDIRECT_BLOCKS_PER_SECTOR ? num_sectors : INDIRECT_BLOCKS_PER_SECTOR);
    }
    inode_dealloc_indirect(&indirect_block.blocks[i], subsize, level -1);
    num_sectors -= subsize;
  }
  free_map_release(block, 1);   //release indirect block itself
  return true;


}

//calls free_map on all data sectors
bool inode_dealloc(struct inode* inode){
  int sectors = bytes_to_sectors(inode->data.length);
   static char zeros[BLOCK_SECTOR_SIZE];   //used for zeroing out blocks
   int i;

   // alloc direct blocks
   int blocks = sectors < DIRECT_BLOCKS_COUNT ? sectors : DIRECT_BLOCKS_COUNT;
   for(i = 0; i < blocks; i++){
      free_map_release(inode->data.direct_blocks[i], 1);
   }
   if((sectors -= blocks) == 0) return true;

   //alloc indirect blocks
   blocks = sectors < INDIRECT_BLOCKS_PER_SECTOR ? sectors : INDIRECT_BLOCKS_PER_SECTOR;
   if(!inode_dealloc_indirect(&inode->data.indirect_block, blocks , 1 ))
    return false;
   if((sectors -= blocks) == 0) return true;

   //alloc doubly indirect blocks
   blocks = sectors < INDIRECT_BLOCKS_PER_SECTOR * INDIRECT_BLOCKS_PER_SECTOR
    ? sectors : INDIRECT_BLOCKS_PER_SECTOR * INDIRECT_BLOCKS_PER_SECTOR;
   if(!inode_dealloc_indirect(&inode->data.doubly_indirect_block, blocks, 2))
    return false;

   if((sectors -= blocks) == 0) return true;
   return false;
}

bool
inode_create (block_sector_t sector, off_t length,bool isDir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      size_t sectors = bytes_to_sectors (length);
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
          //aldair wrote this
          disk_inode->isdir=isDir;
      //shouldn't allocate all sectors at once, but rather 1 at a time
      if (inode_alloc(disk_inode, length))
     // if (free_map_allocate (sectors, &disk_inode->start)) 
        {
          block_write (fs_device, sector, disk_inode);
          //remaining sectors not allocated
          //always created with 1 sector, (length = 0)
          /*if (sectors > 0) 
            {
              static char zeros[BLOCK_SECTOR_SIZE];
              size_t i;
              
              for (i = 0; i < sectors; i++) 
                block_write (fs_device, disk_inode->start + i, zeros);
            }*/
          success = true; 
        } 
      free (disk_inode);
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  block_read (fs_device, inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          free_map_release (inode->sector, 1);
          //free_map_release (inode->data.start,
          //                  bytes_to_sectors (inode->data.length)); 
          inode_dealloc(inode);   
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          block_read (fs_device, sector_idx, buffer + bytes_read);
        }
      else 
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          if (bounce == NULL) 
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          block_read (fs_device, sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  //beyond EOF, file growth
  if(byte_to_sector(inode, offset + size - 1) == -1){   //check this value???
    if(!inode_alloc (& inode->data, offset + size)) return 0;
    inode->data.length = offset + size;
    block_write (fs_device, inode->sector, &inode->data);  //update inode on disk
  } //after this, there should be room to write
  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Write full sector directly to disk. */
          block_write (fs_device, sector_idx, buffer + bytes_written);
        }
      else 
        {
          /* We need a bounce buffer. */
          if (bounce == NULL) 
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
          if (sector_ofs > 0 || chunk_size < sector_left) 
            block_read (fs_device, sector_idx, bounce);
          else
            memset (bounce, 0, BLOCK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          block_write (fs_device, sector_idx, bounce);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  free (bounce);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}
