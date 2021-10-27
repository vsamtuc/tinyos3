
#ifndef _UNIT_TESTING_H
#define _UNIT_TESTING_H


#include "bios.h"
#include "tinyos.h"


/**
	@file unit_testing.h
 	@brief A library for coding and running unit tests.

	
*/


/**
	@defgroup Testing  Unit Testing Library

	@brief A simple but powerful library for writing and executing tests. 


	Overview
	--------

 	Using this library, it is very easy to code unit tests for your code.
 	Let us start with an example.
 	@code
 	#include "unit_testing.h"

 	BARE_TEST(my_test, "This is a silly test")
 	{
		ASSERT(1+1==2);
		ASSERT(2*2*2 < 10);
 	}

	TEST_SUITE(all_my_tests, "These are mine")
	{
		&my_test,
		NULL
	};

	int main(int argc, char** argv) 
	{
		return register_test(&all_my_tests) || 
                run_program(argc, argv, &all_my_tests);
	}
	@endcode

	Reading through the above code should be mostly self-explanatory. 
	You can now compile and run the above program:
	@verbatim
	$ ./test_example 
	running suite: all_my_tests  [cores=1, terminals=0]
	        my_test                           : ok
	        suite all_my_tests completed [tests=1, failed=0]
	all_my_tests                      : ok
	@endverbatim

	It is possible to see the tests that you defined:
	@verbatim
	$ ./test_example -l
	all_tests_available          
	        all_my_tests                 
	                my_test  
	@endverbatim
	
	To see more options, you can run your program with the --help flag.


	Writing tests
	---------------

	A unit test should be testing one aspect of your code at a time. Writing 
	big tests which try to test for everything is not a good practice. Tests
	can be quite simple sometimes, or quite complex, but they should be focused. 
	Also, setting up a test may require you to write other functions (besides 
	the test code). 

	For less experienced programmers, writing test is sometimes hard, because
	their code is not __testable__. Writing testable code requires thought, 
	good programming practices (small, focused functions with a clean API,
	clean data structures, comments) and a willingness to rewrite code in order
	to make it more _beautiful_. Beautiful, well-structured code is by necessity 
	testable.

	Each test is a function, declared in some special way (like in the example,
	where we declared a `BARE_TEST`), so that the library will be able to find it
	and execute it automatically. When a test executes, it can call the code 
	it is testing, and check that it is working correctly, by use of two macros:

	- The @c assert(...)  macro from the standard library. Use this to check conditions
	that, if they fail, will cause the rest of the test to be skipped. For example,
	when we call a routine which returns a pointer to a new object, in order to test it,
	we would probably want to skip the rest of the test if the pointer returned is null.

	- The @c ASSERT(...)  macro is similar to the standard @c assert, but it does not
	cause the rest of the test to be skipped, even if it fails. It does however print
	a very nice message for you to read. Even better, using @c ASSERT_MSG(...) 
	you can have your own message printed, adding more information to help you
	understand better the cause of the problem. Furthermore, when we want to check
	that we should not reach some place, we can use @c FAIL(...) that also prints a
	message.

	It is not a very good practice to print large amounts of messages from a test.
	But, if you really want to print some data, you can use the @c MSG(...) function.
	This function works just like @c printf, but prints the information indented,
	which is neater.

	Running tests
	-------------

	A test may fail for several reasons: some assertion did not hold, some signal 
	such as @c SIGSEGV or @c SIGILL was received, etc. Even if nothing like this
	happens, bugs may still exist, so that after running one test, the process is not
	reliable any more.

	In order to allow more than one test to run in every execution, each test is executed 
	in its own process; that is, before each test function is executed, there is a fork().
	Furthermore, when a test finishes, the process it used is destroyed. In order to guard 
	against tests that may get stuck (for example, in a deadlock), each test is only given
	a predetermined amount of time, which defaults at 10 seconds, but can be changed for
	individual tests that need more time. When this time expires, the process is killed
	and the test is considered a failure.
	In this way, each test runs in isolation, with only minimum overhead (creating a process
	is really fast).  An additional benefit is that test functions need not behave as
	good citizens, e.g., there is no need to clear the memory they allocate, or close any files
	etc.

	Sometimes, we wish to run a test function through the debugger, in order to explore some
	possible bug. Running tests in a different process makes it quite hard to work with
	the debugger. To allow this to happen, we can provide command-line option `--nofork`
	which instructs the library to execute tests in the original process. Usually, we
	would also provide a particular test to run. 
		

	Types of tests
	--------------

	There are different types of tests we may want to run. Some tests may check simple
	data structures or functions. These are **bare tests** and such was the test in the
	previous example.  But the library supports alse **boot tests**, which are tests 
	that run after booting the tinyos kernel. Finally, there is a third type of tests,
	**test suites**. A test suite is just a sequence of other tests (or test suites)
	which will execute one after the other. Test suites are useful for grouping related 
	tests together and giving them a name.

	Boot tests
	-----------

	Boot tests are a bit more complicated to execute that bare tests, because a virtual machine 
	has to boot, with some number of cores and serial devices. By default, boot tests
	are run with one core and no terminals. However, this can be changed by specifying
	at the command line the number of cores and the number of terminals to use. In
	fact, one can specify several combinations to run:
	@verbatim
	$  ./validate_api -c 1,2,3,4 -t 0,2
	@endverbatim
	The above line specifies that every boot test will be run 8 times, once for each
	combination of cores and terminals. 

    ### Terminal proxies.
     
    During boot tests, two functions can be used to test terminal I/O: @ref expect and @ref sendme.
	At the start of a boot test, the library creates a __terminal daemon thread__ for each terminal, which
	can be thought of as a user sitting at this terminal.
	The above functions are requests from the test to this daemon. Function @c expect tells the daemon
	that it should see a particular string on the screen, and @c sendme is a request that the daemon type at
	the keyboard the given string.
    Here is their usage in a test:
    @code
	file1 = OpenTerminal(1);     // open terminal 1

	sendme(1, "hello");

	char buffer[5];
	Read(file1, buffer, 5); 
	ASSERT( memcmp(buffer, "hello", 5) == 0 );

	expect(1, "hi there");
	Write(file1, "hi there", 8);
	@endcode

	Optional test parameters
	------------------------

	Some tests are meaningless when the number of cores or terminals is too small.
	It is possible to designate that such tests should be skipped, by setting 
	optional arguments during test definition. Another optional argument is the
	test timeout (amount of time within which the test must be completed).
	@verbatim
	BOOT_TEST(test_read_kbd_big,
	        "Test that we can read massively from the keyboard on terminal 0.",
	        .minimum_terminals = 1, .timeout = 20)
	{
		// ... stuff ...
	}
	@endverbatim

	@{

 */


/** @brief Maximum number of tests on the command line. */
#define MAX_TESTS 1024

/** @brief Global arguments for test execution */
extern struct program_arguments
{

	/** @brief Flag that we just print the available tests. */
	int show_tests;

	/** @brief Flag verbose */
	int verbose;

	/** @brief Flag use_color */
	int use_color;

	/** @brief Flag to signal fork */
	int fork;

	int ncore_list;		/**< Size of `core_list` */
	/** @brief List with number of cores */
	int core_list[MAX_CORES];

	int nterm_list;		/**< Size of `term_list` */
	/** @brief List with number of terminals */
	int term_list[MAX_TERMINALS+1];

	int ntests;			/**< Size of `tests` */
	/** @brief Tests to run */
	const struct Test* tests[MAX_TESTS];	

} ARGS; /**< The object used to store the program arguments */


/**
	@brief Print formatted messages.

	The messages will be indented to align to the test they belong.
*/
void MSG(const char* format, ...) __attribute__ ((format (printf, 1, 2)));


/** @brief Flag failure during a test */
extern int FLAG_FAILURE; 

/** @brief Like ASSERT but with a custom message. 
	@see ASSERT
*/
#define ASSERT_MSG(expr, format, ...) do{ if(!(expr)) \
 { FLAG_FAILURE=1; MSG(format , ##  __VA_ARGS__ ); } }while(0)

/** 
	@brief Fail the test if an expression is false.

	Much like the lowercase @c assert(...) macro, this macro 
	checks the expression given as argument and fails if it is false.
	Unlike @c assert, this macro does not abort; execution continues.
	However, having set @ref FLAG_FAILURE eventually the test will fail.

	A message with the filename, line and expression that failed is printed
	to the console.
*/
#define ASSERT(expr)  \
 ASSERT_MSG(expr, "%s(%d): ASSERT failed: %s \n",__FILE__, __LINE__, #expr)


/** 
	@brief Fail the test printing a message.

	This macro is similar to @c ASSERT_MSG(0, "...message...").

	A message with the filename and line,  followed by whatever 
	'...messsage...' we provided, is printed to the console.
*/
#define FAIL(failure_message)  \
 ASSERT_MSG(0, "%s(%d): FAILURE: %s \n",__FILE__, __LINE__, (failure_message))



/* 
	Execution utilities
 */

/** @brief Expect to see some bytes printed to terminal.

	This function registers a @c pattern (a string) to
	be expected to appear on the terminal screen. If it
	does not appear, the test fails.

	Subsequent calls to expect add more patterns, that are 
	expected to appear sequentially.
 */
void expect(uint term, const char* pattern);

/** @brief Ask to receive some bytes from the keyboard. 

	This function registers a pattern to be sent from the
	terminal's keyboard. 
*/
void sendme(uint term, const char* pattern);


/** @brief Fill in the bytes of a variable with a weird value:  10101010 or 0xAA 
*/
#define FUDGE(var)  memset(&(var), 170, sizeof(var))


/** @internal
	Test organization
 */
typedef enum { NO_FUNC, BARE_FUNC, BOOT_FUNC, SUITE_FUNC } Test_type;

/** @brief Test descriptor.

	This object describes a test.
 */
typedef struct Test
{
	Test_type type;	    				/**< Bare, boot or suite */
	const char* name;   				/**< Test name */
	union {
		void (*bare)(void*);
		Task boot;
		const struct Test ** suite;
	};									/**< Test function, or list of tests */
	const char* description;			/**< Human-readable, for printing */
	unsigned int timeout;				/**< time to kill test (see DEFAULT_TIMEOUT) */
	unsigned int minimum_terminals;		/**< Minimum no. of terminals required. Default: 0 */
	unsigned int minimum_cores;			/**< Minimum no. of cores required. Default: 1 */
} Test;


/** @brief Default time per test. */
#define DEFAULT_TIMEOUT 10

/** @brief Declare a standard test function. */
#define BARE_TEST(tname, descr, ...) \
static void __test_ ## tname ();   \
const Test tname = { BARE_FUNC, #tname, .bare = __test_##tname , (descr), DEFAULT_TIMEOUT, 0, 1, __VA_ARGS__ }; \
static void __test_ ## tname ()

/** @brief Declare a test function run as the boot function of the tinyos kernel. 
	
	If the user has specified that tests should run on many combinations of cpu cores
	and terminals, this test will be repeated once for each combination.
*/
#define BOOT_TEST(tname, descr, ...) \
static int __test_ ## tname (int, void*); \
const Test tname = { BOOT_FUNC, #tname, .boot = __test_##tname, (descr), DEFAULT_TIMEOUT, 0, 1 , __VA_ARGS__ }; \
static int __test_ ## tname (int argl, void* args)

/** @brief Declare a collectio of test functions.
 */
#define TEST_SUITE(tname, descr, ...) \
extern const Test* __suite_##tname[]; \
const Test tname  = { SUITE_FUNC, #tname, .suite = __suite_##tname, (descr), DEFAULT_TIMEOUT, 0, 1, __VA_ARGS__ }; \
const Test* __suite_##tname[]  = 



/** @brief The main routine to run a test.

	This can be called from the main program or even from within tests.
  */
int run_test(const Test* test);


/** @brief Called from the main program to register a test (usually a suite).

	Typically, this will be called only once, having organized all tests into
	a hierarchy of test suites. However, more than one calls are possible.

	@see run_program
*/
int register_test(const Test* test);

/** @brief Called from the main program to parse arguments and run tests. 

	The typical test program's main routine looks something like this:
	@code
	int main(int argc, char** argv) {
		register_test(all_tests);
		register_test(auxiliary_tests);
		return run_program(argc, argv, all_tests);
	}
	@endcode
*/
int run_program(int argc, char**argv, const Test* default_test);


/** @brief Detect if the test process is found to run under the debugger.

	The detection is done simply by checking that there is a ptrace-attached process
	to this process.

	The current implementation of this function requires certain features specific
	to Linux (procfs in particular) and is not in general portable.

	@return 0 if the process is not attached to another process else non-zero.
  */
int isDebuggerAttached();

/** @} */

#endif