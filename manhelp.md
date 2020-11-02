# NAME
  help - How to get started building TinyOS 3

# SYNOPSIS
```
  make
  make help
  make clean
  make DEBUG=0 clean all
  make depend
```

# DESCRIPTION

## Building tinyos using make

To build your code easily, a complex Makefile is provided. This manual presents several ways to run 
make using this Makefile.

## Basic building

Just type the command
```
$ make
```
This command will compile all the files and build all the executable programs and the examples.
The built programs will contain debugging information, so that you can run the debugger on them.

## Recompiling all the files

When there are weird errors, it is possible that something was corrupted. To remove all binary files
as well as all named pipes, give the following command:
```
$ make clean
```
The you can rebuild everything by giving the command
```
$ make
```

## Building with full optimizations on

To see how fast your code is, you can build with full optimizations. Give the following:
```
$ make DEBUG=0 clean all
```

## Re-making the dependencies

When you change the \#include headers in some file, you should rebuild the dependencies.
```
$ make depend
```
This way you are sure that make re-builds everything it needs to rebuild every time.

# EXTERNAL PROGRAMS

##  Using valgrind

If you have not installed valgrind, the code will be built without support for it. But valgrind is very
useful for debugging. You can install it with the following command:
```
$ sudo apt install valgrind
```

## Building the documentation

The code of tinyos comes with a lot of useful documentation. To build it, you need a program called
'doxygen'. Then, you can view the documentation in your browser. Here is how to install doxygen
```
$ sudo apt install doxygen graphvis
```
Then, you can build the documentation, by the following command:
```
$ make doc
```

