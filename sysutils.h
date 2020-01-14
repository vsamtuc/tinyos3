#ifndef __SYSUTILS_H
#define __SYSUTILS_H

#include "tinyoslib.h"

#define checkargs(argno) if(argc <= (argno)) {\
  printf("Insufficient arguments. %d expected, %zd given.  Try %s -h for help.\n", (argno), argc-1, argv[0]);\
  return 1; }\

void check_help(size_t argc, const char** argv, const char* help);


#endif