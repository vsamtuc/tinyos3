#ifndef BIOS_H
#define BIOS_H

#include <stdint.h>
#include <ucontext.h>

/**
	@file bios.h

	@brief The Virtual Machine API

	This file contains the API for a virtual machine (simulated computer) 
	which we will refer to as VM. This VM is used to implement tinyos3 on.

	The VM has a multicore CPU and peripherals.

	A simulation starts by calling function @c vm_boot. The description of the
	VM (currently, the number of simulated cores and the number of terminal
	devices), and also the initial function executed by each 
	core at boot time, are given as arguments.

	The VM (virtual) hardware is controlled by a BIOS (Basic I/O System) through
	the BIOS API. We now describe the concepts of the BIOS in detail.

	CPU
	---

	A CPU has 1 or more cores. Each core executes independently of each other.
	Variable cpu_core_id contains the id number of the current core.

	Interrupts
	----------

	Each CPU core has its own interrupt vector---it can set its own interrupt 
	handlers, independently of other cores. Setting an interrupt handler
	to @c NULL (the default), ignores the interrupt for this core.

	- When an interrupt handler executes, interrupts are initially disabled.

	- Interrupts can also be enabled and disabled programmatically.

	- If an interrupt is raised while interrupts are disabled, it will be marked as
	raised and the interrupt handler (if non-NULL) will be called as soon as 
	interrupts are re-enabled.

	- The <b>ICI</b> (Inter-Core Interrupt) interrupt can be sent from one core to
	another (or to itself!).

	Peripherals
	-----------

	The peripherals are managed via the 'bios_...' functions. 

	There are two types of simulated peripherals:  _timers_ and _serial ports_ 
	(connected to terminals). Each type of peripheral is documented below.

	Timers
	-------

	Each simulated core has its own timer. A timer can be activated by initializing
	it with some time interval. When the timer expires, the ALARM interrupt is raised 
	for the core.

	Serial ports
	------------- 

	The virtual machine has a number of serial ports connected to terminals.

	Each serial port/terminal can support reading and writing of single bytes.
	The reads return keyboard input, whereas the writes send characters to display
	on the screen.

	Terminals are numbered from 0, up to @c MAX_TERMINALS-1. 

	Implementation-wise, for each terminal/serial port, two 
	Unix named pipes must exist in the current
	directory:  one named  con@f$ N @f$ and one named kbd@f$ N @f$, where @f$ N @f$ is the
	number of the serial port. For example, if the computer has 2 serial ports,
	the following named pipes must be defined in the current directory at runtime:
	con0 kbd0 con1 kbd1

	Also, program  'terminal' must be executed twice (in two different windows):
	
	./terminal 0

	./terminal 1

	Data can be read from  a serial port, one byte at a time. A read
	may fail if the device is not-ready to perform the operation. On a device
	which is ready, the read will succeed. When a non-ready device becomes ready,
	a @c SERIAL_RX_READY interrupt is raised.

	Data can be written to a serial port, one byte at a time. A write
	may fail if the device is not-ready to perform the operation. On a device
	which is ready, the write will succeed. When a non-ready device becomes ready,
	a @c SERIAL_TX_READY interrupt is raised.

	Also, each interrupt is sent if the serial device timeouts (is inactive for
	about 300 msec).

 */


/**
	@brief The signature type of interrupt handlers.
 */
typedef void interrupt_handler();

typedef int sig_atomic_t;

/** @brief Helper declaration */
typedef unsigned int uint;

/** @brief A type for time intervals measured in microseconds */
typedef uint64_t TimerDuration;

/** @brief The interrupts supported by the CPU */
typedef enum Interrupt
{
	ICI,				/**< Raised by some core, via cpu_ici() */
	ALARM,            	/**< Raised when the core's timer expires. */
	SERIAL_RX_READY,	/**< Raised when data is available for reading 
						   from a serial port */
	SERIAL_TX_READY,	/**< Raised when a serial port is ready to accept 
						   data */

	maximum_interrupt_no 
} Interrupt;


/** @brief Maximum number of cores for a virtual machine. */
#define MAX_CORES 32

/** @brief Maximum number of terminals for a virtual machine. */
#define MAX_TERMINALS 4



/**
	@brief Virtual machine configuration

	The configuration of the virtual machine consists of the following
	information:

	- The boot function to execute on each core of the simulated machine, 
	  stored in @c bootfunc.

	- The number of CPU cores of this VM, stored in @c cores

	- The number of serial devices of this VM, stored in @c serialno

	- For each serial device, two file descriptors must be provided: the
	  keyboard (@c serial_in) file descriptor will be read from and the console
	  (@c serial_out) file descriptor will be written to. These file descriptors
	  should correspond to some pipe-like Linux stream (e.g., pipe, FIFO or socket).

 */
typedef struct vm_config {

	/**
		@brief The function executed by each core of the VM at boot.

		When all cores return from this function, the virtual
		machine shuts down.
	 */
	interrupt_handler* bootfunc;


	/**
		@brief The number of CPU cores of the VM.

		The number of cores must be between 1 and @c MAX_CORES.
	 */
	uint cores;


	/** @brief The number of serial ports connected to terminals that
		the computer will support. 

		The number of serial devices should be between 0 and @c MAX_TERMINALS.

		The terminals can be accessed via pipes, which must already exist. 
		The file descriptors for these pipes are passed in the @c kbd and
		@c con arrays of this structure.
	 */
	uint serialno;

	/** @brief The array of file descriptors for input serial ports. 

		Field @c serialno determines the number of file descriptors that
		must be valid in this structure.
	*/
	int serial_in[MAX_TERMINALS];

	/** @brief The array of file descriptors for output serial ports. 

		Field @c serialno determines the number of file descriptors that
		must be valid in this structure.
	*/
	int serial_out[MAX_TERMINALS];
} vm_config;



/**
	@brief Initialize a VM configuration's serial ports using the terminal emulators.

	Set a VM configuration's serial ports by opening the named pipes to connect to
	the terminal emulator program provided in the distribution of @c TinyOS.

	If @c nowait is non-zero, this function will fail and return -1, unless the
	terminal emulators are already running. If @c nowait is zero,
	this function will block until the required terminal emulators are executed.

	In the case of failure, no serial ports will be opened

	@param vmc the configuration to initialize
	@param serialno the number of serial devices to prepare
	@param nowait flag that this function should not block
	@return 0 on success, -1 on failure
*/
int vm_config_terminals(vm_config* vmc, uint serialno, int nowait);


/**
	@brief Initialize a VM configuration with passed parameters.

	Prepare a VM configuration with the given parameters.
	This is a convenience function to initialize the VM configuration
	with serial devices using the terminal emulator program provided 
	in the distribution of @c TinyOS.

	Note that this function will block until the terminal emulators
	are executed.

	@param vmc the configuration to initialize
	@param bootfunc the boot function to execute on cores
	@param cores the number of cores
	@param serialno the number of serial devices
*/
void vm_configure(vm_config* vmc, interrupt_handler bootfunc, uint cores, uint serialno);


/**
	@brief Boot a Virtual Machine with the given configuration.

	This function sets up a virtual machine (VM) according to the
	given configuration.

	The VM boots by executing the function specified
	by the @c bootfunc field of the configuration on each core of the
	simulated VM.

	The execution of the VM ends (and this function returns) when 
	(and if) all cores return from bootfunc, in which case the VM shuts down.

	If the configuration passed contains illegal values, this function will
	print an error message and will @c abort().

	@param vmc the configuration of the virtual machine
	@see vm_config
 */
void vm_run(vm_config* vmc);




/**
	@brief Boot a CPU with the given number of cores and boot function.

	This function sets up a number of simulated cores, each starting to
	execute function bootfunc.

	The number of cores must be between 1 and MAX_CORES.

	Also, this function initializes the simulated peripherals (timers and
	terminals).

	The simulation ends (and this function returns) when (and if) all
	cores return from bootfunc, in which case the VM shuts down.

	@param bootfunc The function that each simulated core will execute at 
			boot time. When all cores return from this function, the virtual
			machine shuts down.
	@param cores The number of cores simulated by the virtual machine
	@param serialno the number of serial ports connected to terminals that
		the computer will support. The terminals can be accessed via named 
		pipes (aka FIFOs), which must already exist. See the serial API below 
		for more details.

 */
void vm_boot(interrupt_handler bootfunc, uint cores, uint serialno);


/**
	@brief Contains the id of the current core.
 */
extern _Thread_local uint cpu_core_id;

/**
   	@brief Returns the number of cores.
 */
uint cpu_cores();


/**
	@brief Barrier synchronization for all cores.

	Each core calling this function stops, until all cores have
	called it. Then, all cores proceed.	

	This is mostly useful when the machine boots the operating
	system, or at shutdown.
 */
void cpu_core_barrier_sync();


/**
	@brief Raise an ICI interrupt to the given core. 

	This is a simple way that one core may interrupt another.
 */
void cpu_ici(uint core);


/**
	@brief Define an interrupt handler for this core.

	This function set the interrupt handler of the calling core, for the given
	interrupt. If @c handler is NULL, then the interrupt will be ignored by this core.

	@param interrupt the interrupt to set the handler for
	@param handler the handler function to call

	@see interrupt_handler
	@see Interrupt
*/
void cpu_interrupt_handler(Interrupt interrupt, interrupt_handler handler);


/**
	@brief Disable interrupts for this core.

	If an interrupt arrives while interrupts are disabled, it will be
	marked as _pending_ and will be raised when interrupts are re-enabled.

	

	@returns 1 if interrupts were enabled before the call, else 0.
	@see cpu_enable_interrupts
 */
int cpu_disable_interrupts();

/**
	@brief Get the current interrupt status for this core.

	@returns 1 if interrupts are enabled, else 0
 */
int cpu_interrupts_enabled();


/**
	@brief Enable interrupts for this core.

	If an interrupt is pending, i.e., it arrived while interrupts were disabled, 
	it will be raised as soon as this call is made.

	@see cpu_disable_interrupts
*/
void cpu_enable_interrupts();


/**
	@brief Halt the core until an interrupt arrives. 

	This function will block the core on which it is called, until an interrupt
	arrives for the core.

	This function is useful when a core becomes idle. An idle core does not
	consume simulation resources (in particular CPU time).
*/
void cpu_core_halt();


/**
	@brief Restart the given core.

	This call will restart the given core, if it was halted.
	@param c the core to restart
*/
void cpu_core_restart(uint c);

/**
	@brief Restart some halted core.

	This call will restart some halted core, if at least one exists.
*/
void cpu_core_restart_one();

/**
	@brief Signal all halted cores to restart.

	When this function is called, all halted cores will be restarted. 
*/
void cpu_core_restart_all();


/**
	@brief A type for saving CPU context into.
*/
typedef ucontext_t cpu_context_t;


/**
	@brief Initialize a CPU context for a new thread.

	To initialize the context, a stack segment of adequate size must be provided.

	@param ctx the context object to initialize
	@param ss_sp the pointer to the beginning of the stack segment
	@param ss_size the size of the stack segment
	@param func the function to execute in the new context
*/
void cpu_initialize_context(cpu_context_t* ctx, void* ss_sp, size_t ss_size, void (*func)());


/**
	@brief Switch the CPU context.

	Save the current context into @c oldctx and load the contents of @c newctx
	into the CPU.

	@param oldctx pointer to the storage for the old context
	@param newctx pointer to the new context to be loaded
*/
void cpu_swap_context(cpu_context_t* oldctx, cpu_context_t* newctx);


/********************************************************************************
 ********************************************************************************/

/** 
	@brief Reset the core timer to the specified interval.

	The interval for the timer is given in microseconds, but the 
	accuracy of the alarm is much coarser, to the order of 10 msec
	(that is, 10,000 microseconds). After the interval expires, the
	core receives an ALARM interrupt.

	This function can be called even if the timer is already activated;
	in this case, the previous timer countdown is canceled and the timer resets
	to the new value.

	If @c usec is specified as 0, any existing timer count is canceled.

	@param usec the timer countdown interval in microseconds
	@returns the time remaining interval since the last call
	@see bios_cancel_timer
 */
TimerDuration bios_set_timer(TimerDuration usec);

/**
	@brief Cancel the current activated timer, if any.

	This can be called even if the timer is not already activated.
	This call is equivalent to bios_set_timer(0).

	@see bios_set_timer
 */
TimerDuration bios_cancel_timer();


/**
	@brief Get the current time from the hardware clock.

	This function returns a real-time clock value, in usec.
	The value of the clock is 10 times the number of seconds since
	the epoch. 

	The resolution of the clock is very low, currently 
	around 100 msec. Therefore, it is inappropriate for any type of
	precise timing.
 */
TimerDuration bios_clock();




/**
	@brief Return the number of serial ports/terminals.

	This is the number specified at the initialization of the
	VM.
 */
uint bios_serial_ports();

/**
	@brief Assign a core to interrupts from a specific serial device.

	Make interrupts of type @c intno for serial port port @c serial be sent
	to @c core.  By default, initially all interrupts are sent to core 0.

	If any parameter has an illegal value, this call has no effect.

	@param serial the serial device whose interrupt is assigned, it must be
	         greater of equal to 
	         @c 0 and less than @c bios_serial_ports().
	@param intno the interrupt to assign (one of @c SERIAL_RX_READY and 
			@c SERIAL_TX_READY)
	@param core the core that will handle this interrupt.
 */
void bios_serial_interrupt_core(uint serial, Interrupt intno, uint core);


/**
	@brief Read a byte from a serial port.

	Try to read a byte from serial port @c serial and store it into the location
	pointed by @c ptr.  If the operation succeds, 1 is returned. If not, 0 is returned.
	The operation may not succeed, if the terminal connected to the serial port has
	not sent any data. 
	
	If this operation returns 0, a @c SERIAL_RX_READY interrupt will be raised when
	data is ready to be received, but the contents of @c *ptr will not be touched.

	@param serial the serial device to read from
	@param ptr the location in which to store the read byte
	@return a integer designating success (non-zero) or failure (zero)
 */
int bios_read_serial(uint serial, char* ptr);


/**
	@brief Write a byte to a serial port.

	Try to write byte @c value to serial port @c serial. If the operation succeds, 
	1 is returned. If not, 0 is returned.

	If this operation returns 0, a @c SERIAL_TX_READY interrupt will be raised when
	the device is ready to accept data.

	@param serial the serial device to write to
	@param value the value to send to the serial device
	@return a integer designating success (non-zero) or failure (zero)
 */
int bios_write_serial(uint serial, char value);


#endif
