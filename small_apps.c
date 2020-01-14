
#include "tinyoslib.h"
#include "symposium.h"


#define checkargs(argno) if(argc <= (argno)) {\
  printf("Insufficient arguments. %d expected, %zd given.  Try %s -h for help.\n", (argno), argc-1, argv[0]);\
  return 1; }\

#define getint(n)  atoi(argv[n])


/*
	Dining philosophers
 */

static void __symp_argproc(size_t argc, const char** argv, symposium_t* symp)
{
	symp->N = getint(1);
	symp->bites = getint(2);

	int dBASE = 0;
	int dGAP = 0;

	if(argc>=4) dBASE = getint(3);
	if(argc>=5) dGAP = getint(4);

	adjust_symposium(symp, dBASE, dGAP);
}

int symp_thr(size_t argc, const char** argv)
{
	checkargs(2);
	symposium_t symp;
	__symp_argproc(argc, argv, &symp);
	return SymposiumOfThreads(sizeof(symp), &symp);
}
REGISTER_PROGRAM(symp_thr)

int symposium(size_t argc, const char** argv)
{
	checkargs(2);
	symposium_t symp;
	__symp_argproc(argc, argv, &symp);
	return SymposiumOfProcesses(sizeof(symp), &symp);
}
REGISTER_PROGRAM(symposium)


int fibonacci(size_t argc, const char** argv)
{
	checkargs(1);
	int n = getint(1);

	printf("Fibonacci(%d)=%d\n", n, fibo(n));
	return 0;
}
REGISTER_PROGRAM(fibonacci)


/*
	Towers of Hanoi
 */

void hanoi_rec(int n, int a, int b, int c)
{
	if(n==0) return;
	hanoi_rec(n-1, a, c, b);
	printf("Move the top disk from tile %2d to tile %2d\n", a, b);
	hanoi_rec(n-1, c, b, a);
}


int hanoi(size_t argc, const char** argv)
{
	checkargs(1);
	int n = getint(1);
	int MAXN = 15;

	if(n<1 || n>MAXN) {
		printf("The argument must be between 1 and %d.\n",MAXN);
		return n;
	}

	printf("We shall move %d disks from tile 1 to tile 2 via tile 3.\n",n);
	hanoi_rec(n, 1,2,3);
	return 0;
}
REGISTER_PROGRAM(hanoi)




TOS_REGISTRY
