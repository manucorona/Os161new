#include <types.h>
#include <thread.h>
#include <current.h>
#include <syscall.h>
#include <addrspace.h>
#include <lib.h>
#include <kern/errno.h>
#include <proc.h>
#include <limits.h>
#include <mips/trapframe.h>
#include <vnode.h>
#include <synch.h>
#include <copyinout.h>
#include <kern/wait.h>


void sys_exit(int exitcode)
{
    int i;
    int j;
// iterate through process table to check pid of the calling process 
    for (i=1; i<PID_MAX ;i++){
        if (process_table[i]!= NULL) {
            if (process_table[i]->proc_id == curproc->proc_id)
                break;
            if (i== PID_MAX-1)
                panic("Current process is not present in the process table");
        }

    }
// abandon the children give to grandparent 
    for (j=1; j<PID_MAX; j++){
        if (process_table[j]!= NULL){
            if (process_table[j]->parent_id == curproc->proc_id){
            lock_acquire(process_table[j]->lock);
            process_table[j]->parent_id = curproc->parent_id;
            lock_release(process_table[j]->lock);
        }
        }
    }
    lock_acquire(curproc->lock);
    curproc->exit_status=true;
    curproc->exit_code = _MKWAIT_EXIT (exitcode);
    KASSERT(curproc->exit_status == process_table[i]->exit_status);
	KASSERT(curproc->exit_code == process_table[i]->exit_code);
	cv_signal(curproc->cv, curproc->lock);
	lock_release(curproc->lock);
	thread_exit();
}





int sys_fork (struct trapframe *tf, pid_t * retval) {
    
    int err = 0;
    struct proc *childProcess;
  

    childProcess = process_create ("Child");
    if (childProcess == NULL) {
        return ENOMEM;
    }

    err = pid_alloc (&childProcess->proc_id);
    if (err) {
        *retval = -1;
        return ENPROC;
    }
    process_table[childProcess->proc_id-1] = childProcess;
    
    
    childProcess->parent_id = curproc->proc_id;

    err = as_copy(curproc->p_addrspace, &childProcess->p_addrspace);
    if (err) {
        *retval = -1;
        return err;
    }
    
    for(int i = 0; i < OPEN_MAX; i++) {
		if(curproc->file_table[i] != NULL){
			lock_acquire(curproc->file_table[i]->lock);
			childProcess->file_table[i] = curproc->file_table[i];
			curproc->file_table[i]->destroy_count++;
			lock_release(curproc->file_table[i]->lock);
		}
	}

    /****************  sync part */
    
    spinlock_acquire(&curproc->p_lock);
    if (curproc->p_cwd != NULL){
        VOP_INCREF(curproc->p_cwd);
        childProcess->p_cwd = curproc->p_cwd;
    }
    spinlock_release(&curproc->p_lock);

    /*************    sync part ****/

    struct trapframe *tfChild = (struct trapframe *) kmalloc(sizeof(struct trapframe));
    *tfChild = *tf;
    if (tfChild == NULL) {
        return ENOMEM;
    }

    err = thread_fork ("Child_thread", childProcess, enter_forked_process, (void *) tfChild, (unsigned long)NULL);
    if (err) {
        kfree(tfChild);
        as_destroy (childProcess->p_addrspace);
        *retval = -1;
        return err;
    }

    *retval = childProcess->proc_id;
    return 0;
}


int sys_waitpid (pid_t pid, int *status, int options, pid_t * retval) {
    int err = 0;
    int i;

    if (curproc->proc_id == pid){
        return ECHILD;
    }
    if(options != 0){
		return EINVAL;
	}
    // check if process with pid exits
    for (i=1;i<PID_MAX;i++){
        if (process_table[i] != NULL){
            if (process_table[i]->proc_id == pid){
                break;
            }
        }
        if (i==PID_MAX-1)
        return ESRCH;
    }
    // check if process with pid  exits
    KASSERT (process_table[i]!= NULL);
    // check if it is the parent process
    if (process_table[i]->parent_id != curproc->proc_id){
        return ECHILD;
    } 

    lock_acquire(process_table[i]->lock);
    // verify if the state is already exit 
    
    if (process_table[i]->exit_status){
        if (status != NULL){
            err = copyout(&process_table[i]->exit_code,(userptr_t) status, sizeof(process_table[i]->exit_code));
            if (err){
                lock_release(process_table[i]->lock);
                proc_destroy(process_table[i]);
                process_table[i]=NULL;
                return err;
            }
        }
        lock_release(process_table[i]->lock);
        *retval=process_table[i]->proc_id;
        proc_destroy(process_table[i]);
        process_table[i]=NULL;
        return 0;        
    } else {
            // if the process has not exited yet we are here
        cv_wait(process_table[i]->cv,process_table[i]->lock);
        if (status != NULL){
        err = copyout(&process_table[i]->exit_code,(userptr_t) status, sizeof(process_table[i]->exit_code));
            if (err){
                lock_release(process_table[i]->lock);
                proc_destroy(process_table[i]);
                process_table[i]=NULL;
                return err;
            }
        }
        lock_release(process_table[i]->lock);
        *retval=process_table[i]->proc_id;
        proc_destroy(process_table[i]);
        process_table[i]=NULL;
        return 0;    
    }
    
}