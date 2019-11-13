#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <setjmp.h>
#include "util.h"
#include "bios.h"

#include "unit_testing.h"

#undef NDEBUG
#include <assert.h>

extern const Test bios_all_tests;

int main(int argc, char** argv)
{
        return register_test(&bios_all_tests) || 
                run_program(argc, argv, &bios_all_tests);
}
