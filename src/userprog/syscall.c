#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "filesys/cache.h"

static void syscall_handler (struct intr_frame *);
static int validate_addr (void *addr);
static void validate_args (void *esp, int argc);

 // Helpers

 // NOTE: get_user and put_user were taken from "3.1.5 Accessing User Memory" in the Project 2 handout
 // but a small tweak was made to them for error checking

 /* Reads a byte at user virtual address UADDR.
    UADDR must be below PHYS_BASE.
    Returns the byte value if successful, -1 if a segfault
    occurred. */
static int
get_user(const uint8_t *uaddr)
{
  bool tmp = is_user_vaddr(uaddr);
  if (!tmp)
  {
    return -1;
  }
  int result;
  asm("movl $1f, %0; movzbl %1, %0; 1:"
      : "=&a"(result)
      : "m"(*uaddr));
  return result;
}

/* Writes BYTE to user address UDST.
   UDST must be below PHYS_BASE.
   Returns true if successful, false if a segfault occurred. */
static bool
put_user(uint8_t *udst, uint8_t byte)
{
  bool tmp = is_user_vaddr(udst);
  if (!tmp)
  {
    return false;
  }
  int error_code;
  asm("movl $1f, %0; movb %b2, %1; 1:"
      : "=&a"(error_code), "=m"(*udst)
      : "q"(byte));
  return error_code != -1;
}

// Ref: From TA Qibo's video on A2
static void
copy_in(void *dst_, const void *usrc_, size_t size)
{
  uint8_t *dst = dst_;
  const uint8_t *usrc = usrc_;

  for (; size > 0; size--, dst++, usrc++)
    *dst = get_user(usrc);
}

/* Validates addr, and exits if it is not a valid user vaddr. */
static int
validate_addr(void *addr)
{
  if (!addr || !is_user_vaddr(addr) || !pagedir_get_page(thread_current()->pagedir, addr))
  {
    sys_exit(-1);
  }
  return 1;
}

/* Validating the address of the arguments of the syscall. */
static void
validate_args(void *esp, int argc)
{
  validate_addr(esp + (argc + 1) * sizeof(void *) - 1);
}


void
syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

 /* 
 Terminates Pintos by calling shutdown_power_off() (declared in devices/shutdown.h). 
 This should be seldom used, because you lose some information about possible deadlock situations, etc.
 */
void
sys_halt (void)
{
  shutdown_power_off ();
}

/* 
 Terminates the current user program, returning status to the kernel. If the process's parent waits for it (see below), 
 this is the status that will be returned. Conventionally, a status of 0 indicates success and nonzero values indicate errors.
 */
void sys_exit(int status)
{
  struct thread *cur = thread_current();

  cur->exit_status->exit_code = status;

  lock_acquire(&cur->exit_status->lock);
  cur->exit_status->ref_count--;
  if (cur->exit_status->ref_count == 0)
    free(cur->exit_status);
  else
    sema_up(&cur->exit_status->sema);
  lock_release(&cur->exit_status->lock);

  while (!list_empty(&cur->child_status_list))
  {
    struct list_elem *e = list_pop_front(&cur->child_status_list);
    struct exit_status_t *exit_status = list_entry(e, struct exit_status_t, elem);
    lock_acquire(&exit_status->lock);
    exit_status->ref_count--;
    if (exit_status->ref_count == 0)
      free(exit_status);
    else
      lock_release(&exit_status->lock);
  }

  printf("%s: exit(%d)\n", (char *)&cur->name, status);
  thread_exit();
}

/*
 Runs the executable whose name is given in cmd_line, passing any given arguments, 
 and returns the new process's program id (pid).
 */
pid_t sys_exec(const char *cmd)
{
  return process_execute(cmd);
}

/*
 Waits for a child process pid and retrieves the child's exit status.
 */
int sys_wait(pid_t pid)
{
  struct thread *cur = thread_current();
  struct list_elem *e;
  for (e = list_begin(&cur->child_status_list); e != list_end(&cur->child_status_list);
       e = list_next(e))
  {
    struct exit_status_t *exit_status = list_entry(e, struct exit_status_t, elem);
    if ((pid_t)exit_status->tid == pid)
    {
      list_remove(e);
      sema_down(&exit_status->sema);

      int exit_code = exit_status->exit_code;

      lock_acquire(&exit_status->lock);
      exit_status->ref_count--;
      if (exit_status->ref_count == 0)
        free(exit_status);
      else
        lock_release(&exit_status->lock);

      return exit_code;
    }
  }
  return -1;
}

/*
 Creates a new file called file initially initial_size bytes in size. 
 Returns true if successful, false otherwise. Creating a new file does not open it: 
 opening the new file is a separate operation which would require a open system call.
 */
bool sys_create(const char *path, unsigned initial_size)
{
  return filesys_create(path, initial_size);
}

/*
 Deletes the file called file. Returns true if successful, false otherwise. 
 A file may be removed regardless of whether it is open or closed, and 
 removing an open file does not close it.
 */
bool sys_remove(const char *path)
{
  return filesys_remove(path);
}

/*
 Opens the file called file. Returns a nonnegative integer handle called a 
 "file descriptor" (fd), or -1 if the file could not be opened.
 */
int sys_open(const char *path)
{
  struct thread *cur = thread_current();
  struct fd_t *fd = malloc(sizeof(struct fd_t));

  if (filesys_open(path, &fd->ptr, &fd->is_dir))
  {
    fd->num = cur->next_fd_num++;
    list_push_back(&cur->fd_list, &fd->elem);
    return fd->num;
  }
  else
  {
    free(fd);
    return -1;
  }
}

/*
 Returns the size, in bytes, of the file open as fd.
 */
int sys_filesize(int fd_num)
{
  struct thread *cur = thread_current();
  struct list_elem *e;
  for (e = list_begin(&cur->fd_list); e != list_end(&cur->fd_list); e = list_next(e))
  {
    struct fd_t *fd = list_entry(e, struct fd_t, elem);
    if (fd->num == fd_num)
    {
      if (!fd->is_dir)
        return file_length((struct file *)fd->ptr);
      else
        return -1;
    }
  }
  return -1;
}

/*
 Reads size bytes from the file open as fd into buffer. 
 Returns the number of bytes actually read (0 at end of file), 
 or -1 if the file could not be read (due to a condition other than end of file).
 */
int sys_read(int fd_num, void *buffer, unsigned size)
{
  if (fd_num == 0)
  {
    unsigned i;
    for (i = 0; i < size; ++i)
      *((uint8_t *)buffer++) = input_getc();
    return size;
  }
  else
  {
    struct thread *cur = thread_current();
    struct list_elem *e;
    for (e = list_begin(&cur->fd_list); e != list_end(&cur->fd_list); e = list_next(e))
    {
      struct fd_t *fd = list_entry(e, struct fd_t, elem);
      if (fd->num == fd_num)
      {
        if (!fd->is_dir)
          return file_read((struct file *)fd->ptr, buffer, size);
        else
          return -1;
      }
    }
    return -1;
  }
}

/*
 Writes size bytes from buffer to the open file fd. Returns the number of bytes actually written, 
 which may be less than size if some bytes could not be written.
 */
int sys_write(int fd_num, const void *buffer, unsigned size)
{
  if (fd_num == 1)
  {
    putbuf(buffer, size);
    return size;
  }
  else
  {
    struct thread *cur = thread_current();
    struct list_elem *e;
    for (e = list_begin(&cur->fd_list); e != list_end(&cur->fd_list); e = list_next(e))
    {
      struct fd_t *fd = list_entry(e, struct fd_t, elem);
      if (fd->num == fd_num)
      {
        if (!fd->is_dir)
          return file_write((struct file *)fd->ptr, buffer, size);
        else
          return -1;
      }
    }
    return -1;
  }
}

/*
 Changes the next byte to be read or written in open file fd to position, 
 expressed in bytes from the beginning of the file.
 */
int sys_seek(int fd_num, unsigned position)
{
  struct thread *cur = thread_current();
  struct list_elem *e;
  for (e = list_begin(&cur->fd_list); e != list_end(&cur->fd_list); e = list_next(e))
  {
    struct fd_t *fd = list_entry(e, struct fd_t, elem);
    if (fd->num == fd_num)
    {
      if (!fd->is_dir)
      {
        file_seek((struct file *)fd->ptr, position);
        return 0;
      }
      else
      {
        return -1;
      }
    }
  }
  return -1;
}

/*
 Returns the position of the next byte to be read or written in open file fd, 
 expressed in bytes from the beginning of the file.
 */
int sys_tell(int fd_num)
{
  struct thread *cur = thread_current();
  struct list_elem *e;
  for (e = list_begin(&cur->fd_list); e != list_end(&cur->fd_list); e = list_next(e))
  {
    struct fd_t *fd = list_entry(e, struct fd_t, elem);
    if (fd->num == fd_num)
    {
      if (!fd->is_dir)
        return file_tell((struct file *)fd->ptr);
      else
        return -1;
    }
  }
  return -1;
}

/*
 Closes file descriptor fd. Exiting or terminating a process implicitly closes all 
 its open file descriptors, as if by calling this function for each one.
 */
int sys_close(int fd_num)
{
  struct thread *cur = thread_current();
  struct list_elem *e;
  for (e = list_begin(&cur->fd_list); e != list_end(&cur->fd_list); e = list_next(e))
  {
    struct fd_t *fd = list_entry(e, struct fd_t, elem);
    if (fd->num == fd_num)
    {
      if (fd->is_dir)
        dir_close((struct dir *)fd->ptr);
      else
        file_close((struct file *)fd->ptr);
      list_remove(e);
      free(fd);
      return 0;
    }
  }
  return -1;
}

/*
Changes the current working directory of the process to dir, 
which may be relative or absolute. Returns true if successful, false on failure.
*/
bool sys_chdir(const char *path)
{
  void *ptr;
  bool is_dir;
  if (filesys_open(path, &ptr, &is_dir))
  {
    if (is_dir)
    {
      dir_close(thread_current()->cwd);
      thread_current()->cwd = (struct dir *)ptr;
      return true;
    }
    else
    {
      file_close((struct file *)ptr);
      return false;
    }
  }
  else
  {
    return false;
  }
}

/*
Creates the directory named dir, which may be relative or absolute. Returns true if successful, false on failure. Fails if dir already 
exists or if any directory name in dir, besides the last, does not already exist.
*/
bool sys_mkdir(const char *path)
{
  char *dir_path, *target;
  split_path(path, &dir_path, &target);

  struct dir *dir;
  bool success = false;
  block_sector_t sector;
  if ((dir = dir_resolve(dir_path)) != NULL)
  {
    sector = 0;
    if (free_map_calloc(&sector) && dir_create(sector, dir_inumber(dir)) && dir_add(dir, target, sector, true))
      success = true;
    dir_close(dir);
  }

  if (!success && sector != 0)
    free_map_release(sector);

  free(dir_path);
  free(target);

  return success;
}

/*
Reads a directory entry from file descriptor fd, which must represent a directory.
If successful, stores the null-terminated file name in name, which must have room
for READDIR_MAX_LEN + 1 bytes, and returns true. If no entries are left in the directory, returns false.
*/
bool sys_readdir(int fd_num, char *name)
{
  struct thread *cur = thread_current();
  struct list_elem *e;
  for (e = list_begin(&cur->fd_list); e != list_end(&cur->fd_list); e = list_next(e))
  {
    struct fd_t *fd = list_entry(e, struct fd_t, elem);
    if (fd->num == fd_num)
    {
      if (fd->is_dir)
        return dir_readdir((struct dir *)fd->ptr, name);
      else
        return false;
    }
  }
  return false;
}

/*
Returns true if fd represents a directory, false if it represents an ordinary file.
*/
bool sys_isdir(int fd_num)
{
  struct thread *cur = thread_current();
  struct list_elem *e;
  for (e = list_begin(&cur->fd_list); e != list_end(&cur->fd_list); e = list_next(e))
  {
    struct fd_t *fd = list_entry(e, struct fd_t, elem);
    if (fd->num == fd_num)
      return fd->is_dir;
  }
  return false;
}

/*
Returns the inode number of the inode associated with fd,
which may represent an ordinary file or a directory.
*/
int sys_inumber(int fd_num)
{
  struct thread *cur = thread_current();
  struct list_elem *e;
  for (e = list_begin(&cur->fd_list); e != list_end(&cur->fd_list); e = list_next(e))
  {
    struct fd_t *fd = list_entry(e, struct fd_t, elem);
    if (fd->num == fd_num)
    {
      if (fd->is_dir)
        return (int)dir_inumber((struct dir *)fd->ptr);
      else
        return (int)file_inumber((struct file *)fd->ptr);
    }
  }
  return -1;
}

/*
Extracts syscall number and arguments from interrupt stack frame, and
executes a valid syscall. In the case of an invalid syscall number, the
exit syscall will be called.
*/
static void
syscall_handler(struct intr_frame *f)
{
  uint32_t *args = ((uint32_t *)f->esp);
  /* Validating ARGS[0] */
  validate_addr((void *)args);
  validate_addr((void *)args + sizeof(void *) - 1);

  char *ptr;
  unsigned i;

  switch (args[0]){
    case SYS_HALT:
      sys_halt();
      break;

    case SYS_EXIT:
      validate_args(f->esp, 1);
      sys_exit((int)args[1]);
      break;

    case SYS_EXEC:
      validate_args(f->esp, 1);
      for (ptr = (char *)args[1]; validate_addr(ptr) && *ptr != '\0'; ++ptr);
      f->eax = sys_exec((char *)args[1]);
      break;

    case SYS_WAIT:
      validate_args(f->esp, 1);
      f->eax = sys_wait((pid_t)args[1]);
      break;

    case SYS_CREATE:
      validate_args(f->esp, 2);
      for (ptr = (char *)args[1]; validate_addr(ptr) && *ptr != '\0'; ++ptr);
      f->eax = sys_create((char *)args[1], (unsigned)args[2]);
      break;

    case SYS_REMOVE:
      validate_args(f->esp, 1);
      for (ptr = (char *)args[1]; validate_addr(ptr) && *ptr != '\0'; ++ptr);
      f->eax = sys_remove((char *)args[1]);
      break;

    case SYS_OPEN:
      validate_args(f->esp, 1);
      for (ptr = (char *)args[1]; validate_addr(ptr) && *ptr != '\0'; ++ptr);
      f->eax = sys_open((char *)args[1]);
      break;

    case SYS_FILESIZE:
      validate_args(f->esp, 1);
      f->eax = sys_filesize((int)args[1]);
      break;

    case SYS_READ:
      validate_args(f->esp, 3);
      for (i = 0; validate_addr((void *)args[2] + i) && i < (unsigned)args[3]; ++i);
      f->eax = sys_read((int)args[1], (void *)args[2], (unsigned)args[3]);
      break;

    case SYS_WRITE:
      validate_args(f->esp, 3);
      for (i = 0; validate_addr((void *)args[2] + i) && i < (unsigned)args[3]; ++i);
      f->eax = sys_write((int)args[1], (void *)args[2], (unsigned)args[3]);
      break;

    case SYS_SEEK:
      validate_args(f->esp, 2);
      f->eax = sys_seek((int)args[1], (unsigned)args[2]);
      break;

    case SYS_TELL:
      validate_args(f->esp, 1);
      f->eax = sys_tell((int)args[1]);
      break;

    case SYS_CLOSE:
      validate_args(f->esp, 1);
      f->eax = sys_close((int)args[1]);
      break;

    case SYS_CHDIR:
      validate_args(f->esp, 1);
      for (ptr = (char *)args[1]; validate_addr(ptr) && *ptr != '\0'; ++ptr);
      f->eax = sys_chdir((char *)args[1]);
      break;

    case SYS_MKDIR:
      validate_args(f->esp, 1);
      for (ptr = (char *)args[1]; validate_addr(ptr) && *ptr != '\0'; ++ptr);
      f->eax = sys_mkdir((char *)args[1]);
      break;

    case SYS_READDIR:
      validate_args(f->esp, 2);
      for (ptr = (char *)args[2]; validate_addr(ptr) && *ptr != '\0'; ++ptr);
      f->eax = sys_readdir((int)args[1], (char *)args[2]);
      break;

    case SYS_ISDIR:
      validate_args(f->esp, 1);
      f->eax = sys_isdir((int)args[1]);
      break;

    case SYS_INUMBER:
      validate_args(f->esp, 1);
      f->eax = sys_inumber((int)args[1]);
      break;

    default:
      sys_exit(-1);
  }
}
