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




int sys_fork(struct trapframe *tf, int* retval)
{
	struct thread* new_thread;
	struct addrspace* child_addrspace;
	int err;

	//start by copying the address  space (virtual memory space of the process) from the parent
	err = as_copy(curthread->t_addrspace, &child_addrspace);
	if(err)
	{
		*retval = -1;
		return err;
	}

	//create a new trapframe in the kernel space and signal if there is not enough memory
	struct trapframe *child_tf = (struct trapframe*)kmalloc(sizeof(struct trapframe));

	*child_tf = *tf;
	if(child_tf == NULL)
	{
		return ENOMEM;
	}

	//call thread_fork, to create a new thread based on an existing one, entrypoint is the func in which he starts to execute
	err = thread_fork("new thread", child_entrypoint, child_tf, child_addrspace, &new_thread);

	if(err)
	{
		kfree(child_tf);
		as_destroy(child_addrspace);
		*retval = -1;
		return err;
	}


	//return value in case of parent: child pid
	*retval = new_thread->pid; 
	return 0;
}

//entry point for the child process
void child_entrypoint(void* data1, long data2) //i don t know why this values
{
	//take the new trapframe from input
	struct trapframe tf;
	tf = (struct trapframe*) data1;

	//load and activate child address space
	curthread->t_addrspace = (struct addrspace*) data2;
	as_activate(curthread->t_addrspace);


	//modify child trapframe in order to signal a success for child
	tf.tf_a3 = 0; //explain here why those values
	tf.tf_v0 = 0; 
	tf.tf_epc += 4; //to avoid to recall the same syscall more times

	//finish, now we can return to user mode
	mips_usermode(&tf);


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
