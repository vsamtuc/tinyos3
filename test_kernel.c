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

	FsMount* mnt = & mount_table[0];

	ASSERT(mnt->fsys == rootfs);
	ASSERT(mnt->mount_point == NULL);

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



struct dl_symbol
{
	const char* name;
	void* addr;
	rlnode symab_node;
	rlnode bindings;
};
struct dl_binding {
	void** addr;
	rlnode bindings_node;
};
struct dl_module
{
	const char* name;
	rdict symtab;
};


typedef struct dlinker
{
	rdict modules;
} dlinker;


void dlinker_init(dlinker* dl)
{
	//rdict_init();
}


void dl_require(dlinker* dl, const char* name)
{

}

void dl_register_symbol(struct dl_module* dlmod, const char* name, void* addr)
{

}

void dl_add_binding(struct dl_module* dlmod, const char* name, void** addr)
{

}

void dl_provide(dlinker* dl, struct dl_module* dlmod);

int dl_resolve(dlinker* dl) 
{

}

#define EXTERN(name) __extern_## name
#define BIND(name)
#define EXPORT(name)


void Lib(dlinker* dl)
{
	int exported_x=0;

	int print_message(void) {
		printf("Hello from lib, x=%d\n", exported_x);
		return 10;
	}

	EXPORT(print_message);
	EXPORT(exported_x);
}


void Prog(dlinker* dl)
{
	int* EXTERN(exported_x);
	BIND(exported_x);
#define exported_x  (*EXTERN(exported_x))

	int (* EXTERN(print_message))(void);
	BIND(print_message);
#define print_message (*EXTERN(print_message))


	int Main()
	{
		exported_x = 10;
		print_message();
		return 0;
	}

	EXPORT(Main);
}




BOOT_TEST(test_dlink,"Test dynamic linking")
{
	/* A dynamic linker */


}






TEST_SUITE(fsystem_tests, "Filesystem tests")
{
	&test_rootfs_is_root,
	&test_pathcomp,
	&test_dlink,
	NULL
};


int main(int argc, char** argv)
{

	return register_test(&fsystem_tests) || 
		run_program(argc, argv, &fsystem_tests);
}
