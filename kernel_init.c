
#ifndef NVALGRIND
#include <valgrind/valgrind.h>
#endif

#include "bios.h"
#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_dev.h"
#include "kernel_streams.h"




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
    initialize_processes();
    initialize_devices();
    initialize_files();
    initialize_scheduler();

    /* The boot task is executed normally! */
    if(Exec(boot_rec.init_task, boot_rec.argl, boot_rec.args)!=1)
      FATAL("The init process does not have PID==1");
  }

  cpu_core_barrier_sync();

#ifndef NVALGRIND
  VALGRIND_PRINTF_BACKTRACE("TINYOS: Entering scheduler for core %d\n",cpu_core_id);
#endif

  run_scheduler();

  if(cpu_core_id==0) {
    /* Here, we could add cleanup after the scheduler has ended. */    
  }
}


void boot(uint ncores, uint nterm, Task boot_task, int argl, void* args)
{
  boot_rec.init_task = boot_task;
  boot_rec.argl = argl;
  boot_rec.args = args;

  vm_boot(boot_tinyos_kernel, ncores, nterm);
}






