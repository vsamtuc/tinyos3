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

	return 0;
}

BOOT_TEST(test_pathcomp, "Test operations on pathcomp_t")
{
	pathcomp_t pcomp;

	strcpy(pcomp, "hello");

	void change(pathcomp_t p) {
		p[0] = 'H';
	}

	change(pcomp);

	ASSERT(pcomp[0]=='H');
	return 0;

}



TEST_SUITE(fsystem_tests, "Filesystem tests")
{
	&test_rootfs_is_root,
	&test_pathcomp,
	NULL
};


int main(int argc, char** argv)
{

	return register_test(&fsystem_tests) || 
		run_program(argc, argv, &fsystem_tests);
}
