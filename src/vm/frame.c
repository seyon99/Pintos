#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"
#include "userprog/syscall.h"
#include "vm/frame.h"

#include <stdlib.h>
#include "filesys/file.h"

//Initialize frame table and any other relevant structures
void initialize_ft (void)
{
    lock_init(&ft_lock);
    list_init(&frame_table);
}


//Tries to obtain a new frame
void* obtain_frame (void) {

    //try to obtain frame
    void* new_frame = palloc_get_page(PAL_USER);
    int size = sizeof(struct frame_table_val);

    //Temporary: Panic if cannot find frame
    if (new_frame == NULL) { 
        PANIC("Error finding free frame");
        return NULL;
    } 

    //Lock used to prevent race condition when modifying frame table data structure
    if (!lock_held_by_current_thread(&ft_lock)) {
        lock_acquire(&ft_lock);
    }
    
    //Allocate memory for new frame table value, set parent and page
    struct frame_table_val *new_ftv = malloc(size);
    new_ftv->parent = thread_current();
    new_ftv->page = new_frame;

    //Push new fram to frame table, release lock
    list_push_back(&frame_table, &new_ftv->elem);
    lock_release(&ft_lock);
        
	return new_frame;
}