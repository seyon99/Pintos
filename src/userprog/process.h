#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

// File descriptor struct for maintaining information
// about a file descriptor
struct fd_table_val
{
    int fd;
    struct list_elem e;
    struct file *file;
};

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);
struct fd_table_val *find_fd(int);

#endif /* userprog/process.h */
