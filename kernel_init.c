
#ifndef NVALGRIND
#include <valgrind/valgrind.h>
#endif

#include "bios.h"
#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_dev.h"
#include "kernel_streams.h"
#include "kernel_sys.h"
#include "kernel_fs.h"


/*
 *
 * Initialization code
 *
 */


/* Parameters from the 'boot' call are passed to boot_tinyos()
   via static variables. */
static struct {
  Task init_task;
  int argl;
  void* args;
} boot_rec;


/* Per-core boot function for tinyos */
void boot_tinyos_kernel()
{

  if(cpu_core_id==0) {
    /* Initialize the kenrel data structures */
    initialize_filesys();
    initialize_processes();
    initialize_devices();
    initialize_files();
    initialize_scheduler();

    /* The boot task is executed normally! */
    if(sys_Spawn(boot_rec.init_task, boot_rec.argl, boot_rec.args)!=1)
      FATAL("The init process does not have PID==1");
  }

  /* Cores wait initialization before entering scheduler */
  cpu_core_barrier_sync();
  
  /* Enter scheduler */
  run_scheduler();

  /* All cores must exit scheduler before finalization */
  cpu_core_barrier_sync();

  if(cpu_core_id==0) {
    /* Here, we could add cleanup after the scheduler has ended. */
    finalize_devices();
    finalize_filesys();
  }

  /* Finalization done, we may shut down */ 
  cpu_core_barrier_sync();
}


void boot(vm_config* vmc, Task boot_task, int argl, void* args)
{
  boot_rec.init_task = boot_task;
  boot_rec.argl = argl;
  boot_rec.args = args;

  vmc->bootfunc = boot_tinyos_kernel; 
  vm_run(vmc);
}






