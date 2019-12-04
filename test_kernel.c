#undef NDEBUG
#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <setjmp.h>
#include "util.h"
#include "bios.h"

#include "unit_testing.h"
#include "kernel_fs.h"


BOOT_TEST(test_rootfs_is_root, "Test that the rootfs filesystem is mounted as root")
{
	FSystem* rootfs = get_fsys("rootfs");

	ASSERT(rootfs!=NULL);	

}



TEST_SUITE(&fsystem_tests, "Filesystem tests")
{
	NULL
};


int main(int argc, char** argv)
{

	return register_test(&sched_tests) || 
		run_program(argc, argv, &sched_tests);
}
