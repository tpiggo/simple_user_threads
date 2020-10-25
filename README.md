# Simple User Level Threading in C

A simple user level threading (SUT) library.

# Uses
Using the simple user level threading library is simple! Simply include the sut.h file into your C program and the world is yours. Functions on a simple FCFS algorithm which allocates CPU time from when the thread gets given access to the CPU until the thread calls yield. The user is in control for when the context switch occurs.

# Requirements
GCC and socket.h
Compile sut.c with the programs which needs the SUT library.
