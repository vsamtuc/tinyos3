
source tinyos-gdb.gdb

echo ****  The GDB extensions for validate-api are loaded\n

define runit
	run --nofork -c 3 test_pipe_multi_producer
end


