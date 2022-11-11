/*
 * Copyright (c) 2013
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Process support.
 *
 * There is (intentionally) not much here; you will need to add stuff
 * and maybe change around what's already present.
 *
 * p_lock is intended to be held when manipulating the pointers in the
 * proc structure, not while doing any significant work with the
 * things they point to. Rearrange this (and/or change it to be a
 * regular lock) as needed.
 *
 * Unless you're implementing multithreaded user processes, the only
 * process that will have more than one thread is the kernel process.
 */

#include <types.h>
#include <spl.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vnode.h>
#include <lib.h>
#include <fcntl.h>
#include <synch.h>
#include <vfs.h>

#include <vm.h>
/*
 * The process for the kernel; this holds all the kernel-only threads.
 */
struct proc *kproc;
struct proc *process_table [PID_MAX];

/*
 * Create a proc structure.
 */
static
struct proc *
proc_create(const char *name)
{
	struct proc *proc;

	proc = kmalloc(sizeof(*proc));
	if (proc == NULL) {
		return NULL;
	}
	proc->p_name = kstrdup(name);
	if (proc->p_name == NULL) {
		kfree(proc);
		return NULL;
	}

	proc->p_numthreads = 0;


	/*initializing empty address space for the process */
	proc->p_addrspace = NULL;

	/* not sure that we need it */
	proc->p_cwd = NULL;
	
	/*initializes process file table as empty*/
	for (int i = 0; i < OPEN_MAX; i++)
		proc->file_table[i] = NULL;

	
	proc->exit_status = false;
	proc->exit_code =-1;
	return proc;
}

/*
 * Destroy a proc structure.
 *
 * Note: nothing currently calls this. Your wait/exit code will
 * probably want to do so.
 */
void
proc_destroy(struct proc *proc)
{
	/*
	 * You probably want to destroy and null out much of the
	 * process (particularly the address space) at exit time if
	 * your wait/exit design calls for the process structure to
	 * hang around beyond process exit. Some wait/exit designs
	 * do, some don't.
	 */

	KASSERT(proc != NULL);
	KASSERT(proc != kproc);

	/*
	 * We don't take p_lock in here because we must have the only
	 * reference to this structure. (Otherwise it would be
	 * incorrect to destroy it.)
	 */

	/* VFS fields */
	if (proc->p_cwd) {
		VOP_DECREF(proc->p_cwd);
		proc->p_cwd = NULL;
	}

	/* VM fields */
	if (proc->p_addrspace) {
		/*
		 * If p is the current process, remove it safely from
		 * p_addrspace before destroying it. This makes sure
		 * we don't try to activate the address space while
		 * it's being destroyed.
		 *
		 * Also explicitly deactivate, because setting the
		 * address space to NULL won't necessarily do that.
		 *
		 * (When the address space is NULL, it means the
		 * process is kernel-only; in that case it is normally
		 * ok if the MMU and MMU- related data structures
		 * still refer to the address space of the last
		 * process that had one. Then you save work if that
		 * process is the next one to run, which isn't
		 * uncommon. However, here we're going to destroy the
		 * address space, so we need to make sure that nothing
		 * in the VM system still refers to it.)
		 *
		 * The call to as_deactivate() must come after we
		 * clear the address space, or a timer interrupt might
		 * reactivate the old address space again behind our
		 * back.
		 *
		 * If p is not the current process, still remove it
		 * from p_addrspace before destroying it as a
		 * precaution. Note that if p is not the current
		 * process, in order to be here p must either have
		 * never run (e.g. cleaning up after fork failed) or
		 * have finished running and exited. It is quite
		 * incorrect to destroy the proc structure of some
		 * random other process while it's still running...
		 */
		struct addrspace *as;

		if (proc == curproc) {
			as = proc_setas(NULL);
			as_deactivate();
		}
		else {
			as = proc->p_addrspace;
			proc->p_addrspace = NULL;
		}
		as_destroy(as);
	}

	for(int i = 3; i < OPEN_MAX; i++) {
        if(proc->file_table[i] != NULL){
            lock_acquire(proc->file_table[i]->lock);
            proc->file_table[i]->destroy_count--;
			if(proc->file_table[i]->destroy_count > 0) {
				lock_release(proc->file_table[i]->lock);
                proc->file_table[i] = NULL;
				//destroy file handle if count reaches 0
        	}else {
				lock_release(proc->file_table[i]->lock);
                KASSERT(proc->file_table[i]->destroy_count == 0);
                lock_destroy(proc->file_table[i]->lock);
                vfs_close(proc->file_table[i]->vnode);
                kfree(proc->file_table[i]);
                proc->file_table[i] = NULL;
        	}

        }
    }

	if (curproc->parent_id == 1){
		for(int i = 0; i < 3; i++) {
        if(proc->file_table[i] != NULL){
            lock_acquire(proc->file_table[i]->lock);
            proc->file_table[i]->destroy_count--;
			if(proc->file_table[i]->destroy_count > 0) {
				lock_release(proc->file_table[i]->lock);
                proc->file_table[i] = NULL;
				//destroy file handle if count reaches 0
        	}else {
				lock_release(proc->file_table[i]->lock);
                KASSERT(proc->file_table[i]->destroy_count == 0);
                lock_destroy(proc->file_table[i]->lock);
                vfs_close(proc->file_table[i]->vnode);
                kfree(proc->file_table[i]);
                proc->file_table[i] = NULL;
        	}

        	}
    	}
	}

	lock_destroy(proc->lock);
	cv_destroy(proc->cv);

	KASSERT(proc->p_numthreads == 0);
	spinlock_cleanup(&proc->p_lock);

	kfree(proc->p_name);
	kfree(proc);
}

/*
 * Create the process structure for the kernel.
 */
void
proc_bootstrap(void)
{
	kproc = proc_create("[kernel]");
	if (kproc == NULL) {
		panic("proc_create for kproc failed\n");
	}
}

/*
 * Create a fresh proc for use by runprogram.
 *
 * It will have no address space and will inherit the current
 * process's (that is, the kernel menu's) current directory.
 */
struct proc *
proc_create_runprogram(const char *name)
{
	struct proc *newproc;
	int err = 0;

	newproc = proc_create(name);
	if (newproc == NULL) {
		return NULL;
	}

	/* VM fields */

	newproc->p_addrspace = NULL;

	/* VFS fields */
	err = pid_alloc (&newproc->proc_id);
	if (err) {
        return NULL;
    }
	newproc->parent_id = curproc->proc_id;
	process_table[newproc->proc_id-1] = newproc;
	for (int i = 0; i < OPEN_MAX; i++)
		newproc->file_table[i] = NULL;

	arg_lock = lock_create("Argument lock");
	KASSERT(arg_lock != NULL);	
	// console initialization STDIN
	err =-1;
	char *con0 = kstrdup("con:");
	KASSERT(con0!=NULL);

	newproc->file_table[0] = (struct file_handle *)kmalloc(sizeof(struct file_handle));
	if (newproc->file_table[0] == NULL) {
		kfree(con0);
		return NULL;
	}

	err = vfs_open(con0, O_RDONLY, 0664, &newproc->file_table[0]->vnode);
	if (err) {
	    kfree(con0);
		kfree(newproc->file_table[0]);
		return NULL;
	}
	kfree(con0);

	newproc->file_table[0]->offset = 0;
	newproc->file_table[0]->con_file = true;
	newproc->file_table[0]->destroy_count = 1;
	newproc->file_table[0]->mode_open = O_RDONLY;
	newproc->file_table[0]->lock = lock_create("STDIN");
	if (newproc->file_table[0]->lock == NULL) {
		kfree(con0);
		vfs_close(newproc->file_table[0]->vnode);
        kfree(newproc->file_table[0]);
		return NULL;
	}


	// console initialization STDOUT
    char *con1 = kstrdup("con:");
       if (con1 == NULL) {
        kfree(con0);
		lock_destroy(newproc->file_table[0]->lock);
		vfs_close(newproc->file_table[0]->vnode);
        kfree(newproc->file_table[0]);
		return NULL;
        }

	err =-1;
	newproc->file_table[1] = (struct file_handle *)kmalloc(sizeof(struct file_handle));
    if (newproc->file_table[1] == NULL) {
		lock_destroy(newproc->file_table[0]->lock);
        vfs_close(newproc->file_table[0]->vnode);
        kfree(newproc->file_table[0]);
        kfree(con0);
		kfree(con1);
        return NULL;
    }
	err = vfs_open(con1, O_WRONLY, 0664, &newproc->file_table[1]->vnode);
    if (err) {
		lock_destroy(newproc->file_table[0]->lock);
        vfs_close(newproc->file_table[0]->vnode);
        kfree(newproc->file_table[0]);
        kfree(newproc->file_table[1]);
		kfree(con0);
		kfree(con1);
        return NULL;
    }
	newproc->file_table[1]->offset = 0;
	newproc->file_table[1]->con_file = true;
	newproc->file_table[1]->destroy_count = 1;
    newproc->file_table[1]->mode_open = O_WRONLY;
    newproc->file_table[1]->lock = lock_create("STDOUT");
    if (newproc->file_table[1]->lock == NULL) {
		lock_destroy(newproc->file_table[0]->lock);
		vfs_close(newproc->file_table[0]->vnode);
        kfree(newproc->file_table[0]);
        vfs_close(newproc->file_table[1]->vnode);
		kfree(con0);
		kfree(con1);
        kfree(newproc->file_table[1]);
		return NULL;
    }

	// console initialization STDERR
	char *con2 = kstrdup("con:");
    if (con2 == NULL) {
        kfree(con0);
        lock_destroy(newproc->file_table[0]->lock);
        vfs_close(newproc->file_table[0]->vnode);
        kfree(newproc->file_table[0]);
        kfree(con1);
        vfs_close(newproc->file_table[1]->vnode);
		lock_destroy(newproc->file_table[1]->lock);
        kfree(newproc->file_table[1]);
		return NULL;
        }
	err =-1;
	newproc->file_table[2] = (struct file_handle *)kmalloc(sizeof(struct file_handle));
	if (newproc->file_table[2] == NULL) {
        lock_destroy(newproc->file_table[0]->lock);
        vfs_close(newproc->file_table[0]->vnode);
        kfree(newproc->file_table[0]);
        vfs_close(newproc->file_table[1]->vnode);
        lock_destroy(newproc->file_table[1]->lock);
        kfree(newproc->file_table[1]);
		kfree(con0);
		kfree(con1);
		kfree(con2);
		return NULL;
    }

	err = -1;
    err = vfs_open(con2, O_WRONLY, 0664, &newproc->file_table[2]->vnode);
	if (err) {
		lock_destroy(newproc->file_table[0]->lock);
		vfs_close(newproc->file_table[0]->vnode);
		kfree(newproc->file_table[0]);
		vfs_close(newproc->file_table[1]->vnode);
		lock_destroy(newproc->file_table[1]->lock);
		kfree(newproc->file_table[1]);
		kfree(con0);
		kfree(con1);
		kfree(con2);
		kfree(newproc->file_table[2]);
        return NULL;
    }

	newproc->file_table[2]->offset = 0;
	newproc->file_table[2]->con_file = true;
    newproc->file_table[2]->destroy_count = 1;
    newproc->file_table[2]->mode_open = O_WRONLY;
	newproc->file_table[2]->lock = lock_create("STDERR");
    if (newproc->file_table[2]->lock == NULL) {
        lock_destroy(newproc->file_table[0]->lock);
        vfs_close(newproc->file_table[0]->vnode);
        kfree(newproc->file_table[0]);
        vfs_close(newproc->file_table[1]->vnode);
		lock_destroy(newproc->file_table[1]->lock);
        kfree(newproc->file_table[1]);
		kfree(con0);
		kfree(con1);
		kfree(con2);
        vfs_close(newproc->file_table[2]->vnode);
		kfree(newproc->file_table[2]);
		return NULL;
    }

	/*
	 * Lock the current process to copy its current directory.
	 * (We don't need to lock the new process, though, as we have
	 * the only reference to it.)
	 */
	spinlock_acquire(&curproc->p_lock);
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		newproc->p_cwd = curproc->p_cwd;
	}
	spinlock_release(&curproc->p_lock);

	return newproc;
}

/*
 * Add a thread to a process. Either the thread or the process might
 * or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
int
proc_addthread(struct proc *proc, struct thread *t)
{
	int spl;

	KASSERT(t->t_proc == NULL);

	spinlock_acquire(&proc->p_lock);
	proc->p_numthreads++;
	spinlock_release(&proc->p_lock);

	spl = splhigh();
	t->t_proc = proc;
	splx(spl);

	return 0;
}

/*
 * Remove a thread from its process. Either the thread or the process
 * might or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
void
proc_remthread(struct thread *t)
{
	struct proc *proc;
	int spl;

	proc = t->t_proc;
	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	KASSERT(proc->p_numthreads > 0);
	proc->p_numthreads--;
	spinlock_release(&proc->p_lock);

	spl = splhigh();
	t->t_proc = NULL;
	splx(spl);
}

/*
 * Fetch the address space of (the current) process.
 *
 * Caution: address spaces aren't refcounted. If you implement
 * multithreaded processes, make sure to set up a refcount scheme or
 * some other method to make this safe. Otherwise the returned address
 * space might disappear under you.
 */
struct addrspace *
proc_getas(void)
{
	struct addrspace *as;
	struct proc *proc = curproc;

	if (proc == NULL) {
		return NULL;
	}

	spinlock_acquire(&proc->p_lock);
	as = proc->p_addrspace;
	spinlock_release(&proc->p_lock);
	return as;
}

/*
 * Change the address space of (the current) process. Return the old
 * one for later restoration or disposal.
 */
struct addrspace *
proc_setas(struct addrspace *newas)
{
	struct addrspace *oldas;
	struct proc *proc = curproc;

	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	oldas = proc->p_addrspace;
	proc->p_addrspace = newas;
	spinlock_release(&proc->p_lock);
	return oldas;
}

//userrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrr
// struct proc* process_create (const char *name){
// 	return proc_create (name);
// }
int process_create(pid_t ppid, pid_t cpid, struct thread * selfThread){

	struct proc * newproc;
	newproc = kmalloc (sizeof(struct proc));
	KASSERT(newproc !=NULL);

	//newproc->cv = waitpidcv;
	newproc->exited = 0;
	newproc->self = selfThread;
	newproc->parent_id = ppid; 
 	process_table[cpid] = newproc;
	return 0;
}

int pid_alloc (pid_t* pid) {
	//first clean up process table of zombies 
	for (int j=1; j<1000 ;j++){
		if (process_table[j]!=NULL){
			if (process_table[j]->exit_status==true && process_table[j]->parent_id ==1 ){	
				proc_destroy(process_table[j]);
        		process_table[j]=NULL;
			}
		}
	}	
    for (int i=1; i<1000 ;i++){
        if (process_table[i]== NULL){
            *pid = i+1;
            return 0;
        }
    }
    return -1; // process table is full
}
