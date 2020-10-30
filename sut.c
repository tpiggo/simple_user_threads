#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include "queue.h"
#include "sut.h"
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <errno.h>

int nextid, livethreads, done_p;
sut_t *running;
pthread_t c_exec, i_exec;
struct queue task_ready, wait, io_to, io_from;
struct queue_entry *c_popped_task, *i_popped_task;
// global locks
static pthread_mutex_t c_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t i_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t from_io_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t io_op_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t i_cond = PTHREAD_COND_INITIALIZER;
// Buffer since only one item can run on the c-exec at once, it is safe to have a global variable that can be returned without malloc
char c_buf[BUFSIZE];

/**
 * The compute execution thread which is a kernel level thread. Runs the FCFS scheduler 
 * @param 
 *      None
 * @return
 *      returns a pthread_exit(0) on  successful completion, errors not handled.
 * 
 **/ 


void *cexec_main(){
    // Busy wait until there is a thread created
	while (1)
    {
        pthread_mutex_lock(&c_lock);
	if (livethreads)
        {
            pthread_mutex_unlock(&c_lock);
            break;
        }
		pthread_mutex_unlock(&c_lock);
    }

	while (1)
    {
        while(1)
        {
            struct queue_entry *popped;
            // Checking if there is something in the queue.
            pthread_mutex_lock(&c_lock);
            if (queue_peek_front(&task_ready)==NULL)
            {
                pthread_mutex_unlock(&c_lock);
                break;
            }
            c_popped_task = queue_pop_head(&task_ready); // Pop the task on the head of the ready queue.
            pthread_mutex_unlock(&c_lock);
            running = (sut_t *) c_popped_task->data;

            // Swap the contexts and start running the next thread!
            swapcontext(running->threadcontext.uc_link, &(running->threadcontext));

            usleep(100);
            if (running->threadexited)
            {
                // Check if the thread exited. If it did, free its memory and move on.
                free(running->threadcontext.uc_link);
                free(running);
                free(c_popped_task);
            }
        }

        // Checking if there are live threads left
        pthread_mutex_lock(&c_lock);
		if (livethreads == 0)
        {
            pthread_mutex_unlock(&c_lock);
            break;
        }
		pthread_mutex_unlock(&c_lock);
	}
    // Signal to the i_exec that we are done!
    pthread_mutex_lock(&i_lock);
    if (!queue_peek_front(&io_to))
        pthread_cond_signal(&i_cond);
    done_p = 1;
    pthread_mutex_unlock(&i_lock);
	pthread_exit(0);
}

/**
 * The io execution thread which is a kernel level thread. Calls the corresponding io procedure when it receives a message
 * over the io queue pipe.
 * @param 
 *      None
 * @return
 *      returns a pthread_exit(0) on  successful completion, errors not handled.
 * 
 **/ 
void *iexec_main(){
    
    while (1)
    {
        pthread_mutex_lock(&c_lock);
		if (livethreads>0)
        {
            pthread_mutex_unlock(&c_lock);
            break;
        }
		pthread_mutex_unlock(&c_lock);
    }
    while (1)
    {
        pthread_mutex_lock(&i_lock);
        // We can guarantee by using an if, no two commands can be done simulataneously. If one command has not finished processing, the next command will
        // Not be serviced.
        if (queue_peek_front(&io_to))
        {
            struct queue_entry *to_head = queue_pop_head(&io_to);
            // Get the message from the queue.
            msg_t *msg_to = (msg_t *)to_head->data;
            if (msg_to->type == 1)
            {
                // Service open command.
                open_msg_t *open = (open_msg_t *)msg_to->msg;
                struct queue_entry *wait_head =  queue_pop_head(&wait);
                
                pthread_mutex_unlock(&i_lock);

                sut_t *task = (sut_t *)wait_head->data;
                struct sockaddr_in server_address = { 0 };

                *(open->sockfd) = socket(AF_INET, SOCK_STREAM, 0);
                
                server_address.sin_family = AF_INET;
                inet_pton(AF_INET, open->ip, &(server_address.sin_addr.s_addr));
                server_address.sin_port = htons(open->port);
                
                if(connect(*(open->sockfd), (struct sockaddr *)&server_address, sizeof(server_address)) < 0)
                {
                    close(*(open->sockfd));
                    // Setting this to -1 in order to ensure it is not a valid file descriptor.
                    *(open->sockfd) = -1;
                    perror("Error opening connection");
                }

                pthread_mutex_lock(&c_lock);
                queue_insert_tail(&task_ready, wait_head);
                pthread_mutex_unlock(&c_lock);

                free(open);

            } 
            else if(msg_to->type == 2)
            {
                // Release io to lock and service close command.
                pthread_mutex_unlock(&i_lock);
                int_msg_t *close_msg = (int_msg_t *)msg_to->msg;

                if(fcntl(*(close_msg->sockfd), F_GETFD))
                    perror("Error closing file");
                else
                    close(*(close_msg->sockfd));
                
                free(close_msg);

            }
            else if(msg_to->type == 3)
            {
                // Service the read command
                int_msg_t *read_msg = (int_msg_t *)msg_to->msg;
                struct queue_entry *wait_head =  queue_pop_head(&wait);
                // release the io to lock
                pthread_mutex_unlock(&i_lock);
                sut_t *task = (sut_t *)wait_head->data;
                msg_t *msg_from = (msg_t *)malloc(sizeof(msg_t));
                msg_from->task_id = msg_to->task_id;
                msg_from->type = 0;
                read_msg_t *read = (read_msg_t *)malloc(sizeof(read_msg_t));
                
                if(fcntl(*(read_msg->sockfd), F_GETFD))
                {
                    perror("Error reading from connection");
                }
                else
                {
                    pthread_mutex_lock(&io_op_lock);
                    recv(*(read_msg->sockfd), read->ret, BUFSIZE, 0);
                    pthread_mutex_unlock(&io_op_lock);
                    
                }
                
                msg_from->msg = (void *)read;
                struct queue_entry *from_node = queue_new_node(msg_from);

                // Double locking to perform the submission of the task and from_io return to be at the same spots in their queues, 
                // thus as we pop we can ensure they will be popped at the same time
                pthread_mutex_lock(&from_io_lock);
                pthread_mutex_lock(&c_lock);

                queue_insert_tail(&io_from, from_node);
                queue_insert_tail(&task_ready, wait_head);

                pthread_mutex_unlock(&from_io_lock);
                pthread_mutex_unlock(&c_lock);
                free(read_msg);

            }
            else
            {
                // Release lock and service the write command.
                pthread_mutex_unlock(&i_lock);
                
                buf_msg_t *write = (buf_msg_t *)msg_to->msg;
        

                if(fcntl(*(write->sockfd), F_GETFD))
                    perror("Error writing to connection");
                else
                {
                    pthread_mutex_lock(&io_op_lock);
                    send(*(write->sockfd), write->message, write->size, 0);
                    pthread_mutex_unlock(&io_op_lock);
                }

                free(write);
            }
            // Both of these were malloced into the memory! Free them.
            free(to_head);
            free(msg_to);
        }
        else
        {
            // Nothing to see, unlock the lock!
            pthread_cond_wait(&i_cond, &i_lock);
            pthread_mutex_unlock(&i_lock);
        }
        
        // Check the flag
        pthread_mutex_lock(&i_lock);
        if (done_p)
        {
            // unlock the lock and breakout
            pthread_mutex_unlock(&i_lock);
            break;
        }
        pthread_mutex_unlock(&i_lock);
    }
    
    pthread_exit(0);
}

void sut_init(){
    nextid = 0;
    livethreads = 0;
    done_p = 0;
    // Initialize the queues.
	task_ready = queue_create();
	queue_init(&task_ready);
	wait = queue_create();
	queue_init(&wait);
    io_from = queue_create();
	queue_init(&io_from);
    io_to = queue_create();
	queue_init(&io_to);
    
	// Start the kernel level POSIX threads.
	if (pthread_create(&c_exec, NULL, cexec_main, NULL)!=0)
    {
		perror("Problem starting thread");
        return;
	}
    if (pthread_create(&i_exec, NULL, iexec_main, NULL)!=0)
    {
		perror("Problem starting thread");
        return;
	}
}

bool sut_create(sut_task_f fn){
    // Check if there are too many threads runnning
    pthread_mutex_lock(&c_lock);
    if (livethreads>=MAX_THREADS)
    {
        printf("Maximum task created.\n");
        pthread_mutex_unlock(&c_lock);
        return false;
    }
    pthread_mutex_unlock(&c_lock);

    sut_t *tdesr = (sut_t *) malloc(sizeof(sut_t));
    if (tdesr == NULL)
    {
        perror("Failed to allocate memory for task.\n");
        return false;
    }
	getcontext(&(tdesr->threadcontext));
    tdesr->file = -1;
	tdesr->threadstack = (char *)malloc(THREAD_STACK_SIZE);
	tdesr->threadcontext.uc_stack.ss_sp = tdesr->threadstack;
	tdesr->threadcontext.uc_stack.ss_size = THREAD_STACK_SIZE;
	tdesr->threadcontext.uc_link = (ucontext_t *)malloc(sizeof(ucontext_t));
	tdesr->threadcontext.uc_stack.ss_flags = 0;
	tdesr->threadfunc = *fn;
    tdesr->threadexited = false;
	makecontext(&(tdesr->threadcontext), *fn, 0);
	struct queue_entry *node = queue_new_node(tdesr);
	
	pthread_mutex_lock(&c_lock);
    
    tdesr->threadid = nextid;
    nextid++;
    livethreads++;
	queue_insert_tail(&task_ready, node);

	pthread_mutex_unlock(&c_lock);

	return true;
}

void sut_yield(){
	pthread_mutex_lock(&c_lock);
	queue_insert_tail(&task_ready, c_popped_task);
	pthread_mutex_unlock(&c_lock);
	swapcontext(&(running->threadcontext), running->threadcontext.uc_link);

}

void sut_exit(){
    // Free malloced memory
    free(running->threadstack);
    // Set the exit value to be true on the thread.
    running->threadexited = true;
    // Lock and update live threads
    pthread_mutex_lock(&c_lock);
	livethreads--;
	pthread_mutex_unlock(&c_lock);
    swapcontext(&(running->threadcontext), running->threadcontext.uc_link);
	
}

void sut_open(char *dest, int port){
    // Make space for sending message.
    msg_t *message = (msg_t *)malloc(sizeof(msg_t));
    open_msg_t *open = (open_msg_t *)malloc(sizeof(open_msg_t));
    // Flag 1: open connection
    message->type = 1;
    message->task_id = running->threadid;
    open->sockfd = &(running->file);
    open->ip = dest;
    open->port = port;
    // pointer type casting for strict typing.
    message->msg = (void *)open;
    struct queue_entry *msg_node = queue_new_node(message);
    pthread_mutex_lock(&i_lock);
    // signal if there is no message on the queue
    if (!queue_peek_front(&io_to))
        pthread_cond_signal(&i_cond);
    queue_insert_tail(&io_to, msg_node);
    
    queue_insert_tail(&wait, c_popped_task);
    pthread_mutex_unlock(&i_lock);
    
    swapcontext(&(running->threadcontext), running->threadcontext.uc_link);

}

void sut_write(char *buf, int size){
    msg_t *message = (msg_t *)malloc(sizeof(msg_t));
    buf_msg_t *write_msg = (buf_msg_t *)malloc(sizeof(buf_msg_t));
    // Flag: write message
    message->type = 0;
    message->task_id = running->threadid;
    write_msg->sockfd = &(running->file);
    write_msg->message = buf;
    write_msg->size = size;
    // pointer type casting for strict typing.
    message->msg = (void *)write_msg;
    struct queue_entry *node = queue_new_node(message);
    pthread_mutex_lock(&i_lock);
    // signal if there is no message on the queue
    if (!queue_peek_front(&io_to))
        pthread_cond_signal(&i_cond);
    queue_insert_tail(&io_to, node);
    pthread_mutex_unlock(&i_lock);
}

void sut_close(){
    // Non-blocking close of the file descriptor connected to the task. 
    msg_t *message = (msg_t *)malloc(sizeof(msg_t));
    int_msg_t *close_msg = (int_msg_t *)malloc(sizeof(int_msg_t));
    // Flag: close message
    message->type = 2;
    message->task_id = running->threadid;
    close_msg->sockfd = &(running->file);
    message->msg = (void *) close_msg;
    // No internal message to be passed.
    struct queue_entry *node = queue_new_node(message);
    pthread_mutex_lock(&i_lock);
    // Check if there is anything on the queue, if not signal
    if (!queue_peek_front(&io_to))
        pthread_cond_signal(&i_cond);
    queue_insert_tail(&io_to, node);
    pthread_mutex_unlock(&i_lock);
}

char *sut_read(){
    msg_t *message = (msg_t *)malloc(sizeof(msg_t));
    int_msg_t *read = (int_msg_t *)malloc(sizeof(int_msg_t));
    // Flag: Read message
    message->type = 3;
    message->task_id = running->threadid;
    read->sockfd = &(running->file);
    message->msg = (void *) read;

    struct queue_entry *msg_node = queue_new_node(message);
    pthread_mutex_lock(&i_lock);
    // Check if there is anything on the queue, if not signal
    if (!queue_peek_front(&io_to))
        pthread_cond_signal(&i_cond);
    queue_insert_tail(&io_to, msg_node);
    queue_insert_tail(&wait, c_popped_task);
    pthread_mutex_unlock(&i_lock);
    // return power to C-exec.
    swapcontext(&(running->threadcontext), running->threadcontext.uc_link);
    // After having been reschuduled, you continue execution of the read

    pthread_mutex_lock(&from_io_lock);
    msg_node = queue_pop_head(&io_from);
    pthread_mutex_unlock(&from_io_lock);
    message = (msg_t *)msg_node->data;
    read_msg_t *msg_ret = (read_msg_t *) message->msg;
    memcpy(&c_buf, msg_ret->ret, BUFSIZE);
    free(msg_ret);
    free(msg_node);
    free(message);
	return c_buf;
}

void sut_shutdown(){
    // Shutdown the kernel threads.
	pthread_join(c_exec, NULL);
    pthread_join(i_exec, NULL);
	printf("Threading done! Shutting down.\n");
}
