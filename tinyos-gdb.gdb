
echo ****  The GDB extensions for tinyos v3 are loaded\n

#
#  Show the current process table
#

define info process-table
	set $i=1
	echo =================\nActive processes\n------------------\n
	printf "%5s %5s %18s\n","PID","PPID","Program addr"
	while $i < 65536
		if PT[$i].pstate == ALIVE
			printf "%5d %5d %18p \n" , get_pid(&PT[$i]), get_pid(PT[$i].parent), PT[$i].main_task
		end
		set $i=$i+1
	end
end

document info process-table
Show a list of the alive processes with information per process.
end


define core
	if $argc==1
		eval "thread %d\n", $arg0+2
	else
		printf "The VM has %d cores.\n", ncores
	end
end
document core
Switch to a VM core.
After this command, the state of the current thread on this core can
be examined.
Without arguments, it just prints the number of VM cores.
end


define info VM
	if ncores==0
		echo The VM is not running\n
	else
		printf "VM cores=%3d   serial devices=%3d\n", ncores, nterm
		echo  Pending interrupts\n
		set $i=0
		while $i<ncores
			printf "Core %3d  [%7s, irq=%10s]: \t", $i, CORE[$i].halted?"HALTED":"RUNNING" , CORE[$i].int_disabled?"DISABLED":"ENABLED "
			if CORE[$i].intpending[0]
				echo ICI\t
			end
			if CORE[$i].intpending[1]
				echo ALARM\t
			end
			if CORE[$i].intpending[2]
				echo SERIAL_RX_READY\t 
			end
			if CORE[$i].intpending[3]
				echo SERIAL_TX_READY\t 
			end
			echo \n
			set $i=$i+1
		end			
	end
end
document info VM
	Show information on the configuration of the Virtual Machine.
end

