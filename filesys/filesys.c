#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "threads/thread.h"
#include "threads/malloc.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();

  if (format) 
    do_format ();

  free_map_open ();
      thread_current()->currentDir=dir_open_root();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size, bool isDir) 
{
  block_sector_t inode_sector = 0;
  //look for directory for the file//
  int i=0;
  if(name[i]==NULL){return false;}
  //moving a directory that is inside our current directory//
  struct thread* t= thread_current();
  struct dir* currentDir=NULL;
  if(name[i]!='/'){
    currentDir=t->currentDir;
  }
  //moving to the root first and then look for directory//
  else {
    currentDir=dir_open_root();
  }
  char* file =(char*)malloc(sizeof(char)*strlen(name));
  struct inode* currentInode;
  int k=0;
  bool finalDir=false;
  while(name[i]!=NULL){
    if(name[i]=='.' && name[i+1]=='.'){
        i=i+2;
        file[0]='.';
        file[1]='.';
        dir_lookup (currentDir, file,&currentInode);
        if(currentInode==NULL){return false;}
        currentDir=dir_open(currentInode);
        if(currentDir==NULL){return false;}

    }
    else if (name[i]=='/'){
      if(k==0){
        file[k]=0;
        dir_lookup (currentDir, file,&currentInode);
        if(currentInode==NULL){return false;}
        currentDir=dir_open(currentInode);
        if(currentDir==NULL){return false;}
        k=0;
        finalDir=false;
      } 
      i++;
    }
    else{
      file[k]=name[i];
      i++;
      k++;
    }

  }


  bool success = (currentDir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size,isDir)
                  && dir_add (currentDir, file, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (currentDir);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{

  int i=0;
  if(name[i]==NULL){return false;}
  //moving a directory that is inside our current directory//
  struct thread* t= thread_current();
  struct dir* currentDir=NULL;
  if(name[i]!='/'){
    currentDir=t->currentDir;
  }
  //moving to the root first and then look for directory//
  else {
    currentDir=dir_open_root();
  }
  char* file =(char*)malloc(sizeof(char)*strlen(name));
  struct inode* currentInode;
  int k=0;
  bool finalDir=false;
  while(name[i]!=NULL){
    if(name[i]=='.' && name[i+1]=='.'){
        i=i+2;
        file[0]='.';
        file[1]='.';
        dir_lookup (currentDir, file,&currentInode);
        if(currentInode==NULL){return false;}
        currentDir=dir_open(currentInode);
        if(currentDir==NULL){return false;}

    }
    else if (name[i]=='/'){
      if(k==0){
        file[k]=0;
        dir_lookup (currentDir, file,&currentInode);
        if(currentInode==NULL){return false;}
        currentDir=dir_open(currentInode);
        if(currentDir==NULL){return false;}
        k=0;
        finalDir=false;
      } 
      i++;
    }
    else{
      file[k]=name[i];
      i++;
      k++;
    }

  }
  struct inode *inode = NULL;

  if (currentDir != NULL)
    dir_lookup (currentDir, name, &inode);
  dir_close (currentDir);

  return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  int i=0;
  if(name[i]==NULL){return false;}
  //moving a directory that is inside our current directory//
  struct thread* t= thread_current();
  struct dir* currentDir=NULL;
  if(name[i]!='/'){
    currentDir=t->currentDir;
  }
  //moving to the root first and then look for directory//
  else {
    currentDir=dir_open_root();
  }
  char* file =(char*)malloc(sizeof(char)*strlen(name));
  struct inode* currentInode;
  int k=0;
  bool finalDir=false;
  while(name[i]!=NULL){
    if(name[i]=='.' && name[i+1]=='.'){
        i=i+2;
        file[0]='.';
        file[1]='.';
        dir_lookup (currentDir, file,&currentInode);
        if(currentInode==NULL){return false;}
        currentDir=dir_open(currentInode);
        if(currentDir==NULL){return false;}

    }
    else if (name[i]=='/'){
      if(k==0){
        file[k]=0;
        dir_lookup (currentDir, file,&currentInode);
        if(currentInode==NULL){return false;}
        currentDir=dir_open(currentInode);
        if(currentDir==NULL){return false;}
        k=0;
        finalDir=false;
      } 
      i++;
    }
    else{
      file[k]=name[i];
      i++;
      k++;
    }

  }
  //can only remove dir if it is empty
  if(currentDir->inode->numEntries==0){
    bool success = currentDir != NULL && dir_remove (currentDir, file);
    dir_close (currentDir); 
    return success;
  }
  return false;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}
