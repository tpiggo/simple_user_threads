#ifndef __SUT_H__
#define __SUT_H__
#include <stdbool.h>
#include <ucontext.h>

#define MAX_THREADS         100
#define THREAD_STACK_SIZE   1024*64
#define BUFSIZE             128

typedef void (*sut_task_f)();

typedef struct __sut
{
	int threadid;
	char *threadstack;
	void *threadfunc;
    int threadexited;
    int file;
	ucontext_t threadcontext;
    ucontext_t parent;
} sut_t;

typedef struct __open_msg{
    int port;
    char *ip;
    int *sockfd;
} open_msg_t;

typedef struct __buf_msg{
    char *message;
    int size;
    int *sockfd;
}buf_msg_t;

typedef struct __int_msg{
    int *sockfd;
} int_msg_t;

typedef struct __read_msg{
    char ret[BUFSIZE];
}read_msg_t;

typedef struct __msg_t{
    int type;
    int task_id;
    void *msg;
} msg_t;

void sut_init();
bool sut_create(sut_task_f fn);
void sut_yield();
void sut_exit();
void sut_open(char *dest, int port);
void sut_write(char *buf, int size);
void sut_close();
char *sut_read();
void sut_shutdown();


#endif
