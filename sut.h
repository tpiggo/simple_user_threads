#ifndef __SUT_H__
#define __SUT_H__
#include <stdbool.h>
#include <ucontext.h>

#define MAX_THREADS         100
#define THREAD_STACK_SIZE   1024*64
#define BUFSIZE             128


typedef void (*sut_task_f)();

/**
 * TCB for the simple user level threads.
 **/
typedef struct __sut
{
	int threadid;
	char *threadstack;
	void *threadfunc;
    int threadexited;
    int file;
	ucontext_t threadcontext;
} sut_t;

/**
 * Open message.
 **/
typedef struct __open_msg{
    int port;
    char *ip;
    int *sockfd;
} open_msg_t;

/**
 * buffered message for writing.
 **/
typedef struct __buf_msg{
    char *message;
    int size;
    int *sockfd;
}buf_msg_t;

/**
 * integer message, used for closing or reading.
 **/
typedef struct __int_msg{
    int *sockfd;
} int_msg_t;

/**
 * Standardized message for the io queues to pass.
 **/
typedef struct __read_msg{
    char ret[BUFSIZE];
}read_msg_t;


/**
 * Standardized message for the io queues to pass.
 **/
typedef struct __msg_t{
    int type;
    int task_id;
    void *msg;
} msg_t;

/**
 * Initialize the simple user level threading library. 
 * @param None.
 * @return pthread_exit(0) on  successful completion, errors not handled.
 * 
 **/ 
void sut_init();

/**
 * Creating a user level task which will be placed into the task  ready queue at completion and scheduled by
 * the compute execution thread.
 * @param Function type of sut_task_f.
 * @return returns true on successful creation of the task, false if an error occurred.
 * 
 **/ 
bool sut_create(sut_task_f fn);

/**
 * Handling the replacement of the task onto the task ready queue and returning control to the compute execution thread. Return control by using
 * swapcontext from the current user level thread into the kernel thread.
 * @param None.
 * @return void.
 * 
 **/ 
void sut_yield();

/**
 * Handling the disassembly of the task as it finishes it execution and returning control to the compute execution thread. Return control by using
 * swapcontext from the current user level thread into the kernel thread.
 * @param None.
 * @return void.
 * 
 **/ 
void sut_exit();

/**
 * Opens a connection to a remote process using sockets. Passing a message from compute execution thread to the IO execution thread, the connection
 * is opened while the task waits for the process to be completed. Is subsequently replaced onto the task ready queue at completion. Blocking operation
 * @param destination of the socket, port number to bind.
 * @return void however can cause a perror during execution.
 * 
 **/ 
void sut_open(char *dest, int port);

/**
 * Writing to the remote process which was opened at the call of sut_open. Errors handled IO thread which checks if there is a successful open before
 * calling the send function. None blocking operation.
 * @param buf is a character string, size is the size of the string as an int.
 * @return void, perror if an invalid file descriptor is passed.
 * 
 **/ 
void sut_write(char *buf, int size);

/**
 * Closing a connection to the remote process which was opened at the call of sut_open. Errors handled IO thread which checks if there is a successful
 * open before calling the send function. None blocking operation.
 * @param None.
 * @return void, perror if an invalid file descriptor is passed.
 * 
 **/ 
void sut_close();

/**
 * Reading a response from the remote process which was opened at the call of sut_open. Task which is calling read is moved from the wait queue back onto
 * task ready queue at the completion of the io operation. Errors handled IO thread which checks if there is a successful open before calling the send 
 * function. Blocking operation.
 * @param None.
 * @return void, perror if an invalid file descriptor is passed.
 * 
 **/ 
char *sut_read();

/**
 * Calls pthread join and waits for the pthreads to finish execution. Blocking operation.
 * @param 
 * @return pthread_exit(0) on  successful completion, errors not handled.
 * 
 **/ 
void sut_shutdown();


#endif
