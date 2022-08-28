#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <list.h>

struct lock ft_lock;
struct list frame_table;

struct frame_table_val 
{
    struct thread* parent;
    void* page;
    struct list_elem elem;
};

void initialize_ft(void);
void* obtain_frame (void) 

 #endif /* userprog/syscall.h */