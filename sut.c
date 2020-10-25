#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include "queue/queue.h"
#include "sut.h"
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>

int nextid, livethreads, done_p;
sut_t *running;
struct queue task_ready, wait, io_to, io_from;
pthread_t c_exec, i_exec;
//Global lock is needed for when the two threads want to touch the queue
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t i_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t icond = PTHREAD_COND_INITIALIZER;

void *cexec_main(void *args){
	// This is the main kernel level thread which does the C-exec
	printf("Started the C-EXEC!\n");
    // Busy wait until there is a thread created
	while (1){
        pthread_mutex_lock(&mutex);
		if (livethreads){
            pthread_mutex_unlock(&mutex);
            break;
        }
		pthread_mutex_unlock(&mutex);
    }
	while (1){
        while(1){
            struct queue_entry *popped;
            // Acquire the lock and get the next task
            pthread_mutex_lock(&mutex);
            // If there is no tasks, break out and check if there are live threads.
            if (!queue_peek_front(&task_ready)){
                pthread_mutex_unlock(&mutex);
                break;
            }
            popped = queue_pop_head(&task_ready); // Pop the task on the head of the ready queue.
            pthread_mutex_unlock(&mutex);
            running = popped->data;
            // Swap the contexts and start running the next thread!
            swapcontext(&(running->parent), &(running->threadcontext));
            // Per assignment requirements, issue a usleep for 100 microseconds 
            usleep(100);
            // Since we are readding it to the queue using queue_new_node, we can be sure it is safe to free
            free(popped);
            if (running->threadexited){
                // Check if the thread exited. If it did, free its memory and move on.
                free(running);
            }
        }
        // Acquire the lock and check if there are live threads. If so, continue if not you're done.
        pthread_mutex_lock(&mutex);
		if (!livethreads){
            pthread_mutex_unlock(&mutex);
            break;
        }
		pthread_mutex_unlock(&mutex);
	}
    
	printf("Exiting C-EXEC\n");
    // Signal to the i_exec that we are done!
    pthread_mutex_lock(&i_lock);
    done_p = 1;
    pthread_mutex_unlock(&i_lock);
	pthread_exit(0);
}

void *iexec_main(){
    printf("Starting I-EXEC\n");
    while (1){
        pthread_mutex_lock(&mutex);
		if (livethreads){
            pthread_mutex_unlock(&mutex);
            break;
        }
		pthread_mutex_unlock(&mutex);
    }
    while (1){
        pthread_mutex_lock(&i_lock);
        if (queue_peek_front(&io_to)){
            // If there is a io_to waiting, handle
            struct queue_entry *to_head = queue_pop_head(&io_to);
            // Get the message from the queue.
            msg_t *msg_to = (msg_t *)to_head->data;
            if (msg_to->type == 1){
                // Service open command.
                struct queue_entry *wait_head =  queue_peek_front(&wait);
                sut_t *task = wait_head->data;
                // Ensure the first element on the io_queue is the same task as the element on the qait queue. If they are not the same, then this task is some other type 
                if (task->threadid == msg_to->task->threadid){
                    /**
                     * Only on confirmation that both the task wait and io message in are the same task can we pop the head of the task wait.
                     * This means we
                    **/
                    open_msg_t *open = (open_msg_t *)msg_to->msg;
                    wait_head =  queue_pop_head(&wait);
                    task = (sut_t *)wait_head->data;
                    struct sockaddr_in server_address = { 0 };

                    // create a new socket
                    msg_to->task->file = socket(AF_INET, SOCK_STREAM, 0);

                    // connect to server
                    server_address.sin_family = AF_INET;
                    inet_pton(AF_INET, open->ip, &(server_address.sin_addr.s_addr));
                    server_address.sin_port = htons(open->port);
                    if(connect(msg_to->task->file, (struct sockaddr *)&server_address, sizeof(server_address)) < 0){
                        printf("Problem opening connection!\n");
                    }
                    free(wait_head);
                    struct queue_entry *task_node = queue_new_node(task);
                    pthread_mutex_lock(&mutex);
                    queue_insert_tail(&task_ready, task_node);
                    pthread_mutex_unlock(&mutex);
                    free(open);
                }
            } else if(msg_to->type == 2){
                // Service close command.
                close(msg_to->task->file);
            }else {
                /**
                 * Service the write command.
                 * Sending data over the server to the socket opened in the file. Use  MSG_NOSIGNAL in the handling of errors. As per requirements of the assignment,
                 * We do not handle errors. Thus we choose to ignore the errors on send thus allowing the execution of the program to continue.
                **/
                buf_msg_t *open = (buf_msg_t *)msg_to->msg;
                if (send(msg_to->task->file, open->message, open->size, MSG_NOSIGNAL)<0){
                    printf("Failed to send\n");
                }
                free(open);
            }
            // Both of these were malloced into the memory! Free them.
            free(to_head);
            free(msg_to);
        }
        // Release the lock. Releasing after the write is to ensure the read process cannot begin before the write is finished. 
        // Releasing after open connection has been serviced as well therefore, we can ensure no race condition on the socket!
        pthread_mutex_unlock(&i_lock);
        /**
         * TODO: Service a read!
         **/ 

        // We are running until we get a signal that we are done.
        pthread_mutex_lock(&i_lock);
        if (done_p){
            // unlock the lock and breakout
            pthread_mutex_unlock(&i_lock);
            break;
        }
        pthread_mutex_unlock(&i_lock);
    }
    printf("Exiting I-EXEC\n");
    pthread_exit(0);
}

void sut_init(){
    // Nextid is a counter for the next id of a thread. No race conditions, it will only be accessed in sut_create.
    nextid = 0;
    //livethreads is a counter for the number of live threads. This can have race conditions, thus will be part of the CS.
    livethreads = 0;
    // Set exit to 0
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
	if (pthread_create(&c_exec, NULL, cexec_main, NULL)!=0){
		printf("Problem starting thread!\n");
        return;
	}
    if (pthread_create(&i_exec, NULL, iexec_main, NULL)!=0){
		printf("Problem starting thread!\n");
        return;
	}
}

bool sut_create(sut_task_f fn){
    // Check if there are too many threads runnning
    // Acquire lock and check live threads
    pthread_mutex_lock(&mutex);
    if (livethreads>=MAX_THREADS){
        printf("Maximum task created.\n");
        pthread_mutex_unlock(&mutex);
        return false;
    }
    pthread_mutex_unlock(&mutex);

    sut_t *tdesr = (sut_t *) malloc(sizeof(sut_t));
    if (!tdesr){
        printf("Failed to allocate memory for task.\n");
        return false;
    }
	getcontext(&(tdesr->threadcontext));
    // Hold the file descriptor for the task
    tdesr->file = 0;
	tdesr->threadstack = (char *)malloc(THREAD_STACK_SIZE);
	tdesr->threadcontext.uc_stack.ss_sp = tdesr->threadstack;
	tdesr->threadcontext.uc_stack.ss_size = THREAD_STACK_SIZE;
	tdesr->threadcontext.uc_link = 0;
	tdesr->threadcontext.uc_stack.ss_flags = 0;
	tdesr->threadfunc = *fn;
    tdesr->threadexited = false;
	makecontext(&(tdesr->threadcontext), *fn, 0);
	struct queue_entry *node = queue_new_node(tdesr);
	// Acquire the lock and then add to the queue then release the lock. CS!!
	pthread_mutex_lock(&mutex);
    // Critical section: Cannot have two threads with the same id
    tdesr->threadid = nextid;
    nextid++;
    // Increase number of live threads by 1.
    livethreads++;
	queue_insert_tail(&task_ready, node);
	pthread_mutex_unlock(&mutex);
	return true;
}
void sut_yield(){
	// Put yourself back onto the ready queue since you yielded, you're not done yet.
	struct queue_entry *node = queue_new_node(running);
	pthread_mutex_lock(&mutex);
	queue_insert_tail(&task_ready, node);
	pthread_mutex_unlock(&mutex);
    // Problem: Cannot return to the prent like this
    //--> if you're coming back from i-exec, you mustn't force 2 versions of c-exec to be running :S
	swapcontext(&(running->threadcontext), &(running->parent));

}
void sut_exit(){
    // This function needs to context swtich back to the parent and release its malloced memory
    // Do not put this task back into the task ready queue! It is done.
    free(running->threadstack);
    // Set the exit value to be true on the thread.
    running->threadexited = true;
    // Critical section: We must update the number of live threads, reduce by 1.
    pthread_mutex_lock(&mutex);
	livethreads--;
	pthread_mutex_unlock(&mutex);
    swapcontext(&(running->threadcontext), &(running->parent));
	
}
void sut_open(char *dest, int port){
    // Send a message to the i-exec saying you want to 
    msg_t *message = (msg_t *)malloc(sizeof(msg_t));
    open_msg_t *open = (open_msg_t *)malloc(sizeof(open_msg_t));
    // Flag 1: open connection
    message->type = 1;
    message->task = running;
    open->ip = dest;
    open->port = port;
    message->msg = (void *)open;
    printf("Made it here\n");
    struct queue_entry *msg_node = queue_new_node(message);
    struct queue_entry *task_node = queue_new_node(running);
    // Signal to the i_exec that we are done!
    pthread_mutex_lock(&i_lock);
    queue_insert_tail(&io_to, msg_node);
    queue_insert_tail(&wait, task_node);
    pthread_mutex_unlock(&i_lock);
    // Once the message has been sent, allow C-exec to schedule a new task. Return control to c-exec.
    // As per assignment requirements.
    swapcontext(&(running->threadcontext), &(running->parent));

}
void sut_write(char *buf, int size){
    msg_t *message = (msg_t *)malloc(sizeof(msg_t));
    buf_msg_t *open = (buf_msg_t *)malloc(sizeof(buf_msg_t));
    message->type = 0;
    message->task = running;
    open->message = buf;
    open->size = size;
    // Cast the buf message to void type in order to be put into the message param of msg_t pointer.
    message->msg = (void *)open;
    struct queue_entry *node = queue_new_node(message);
    pthread_mutex_lock(&i_lock);
    queue_insert_tail(&io_to, node);
    pthread_mutex_unlock(&i_lock);
}
void sut_close(){
    // Non-blocking close of the file descriptor connected to the task. 
    msg_t *message = (msg_t *)malloc(sizeof(msg_t));
    message->type = 2;
    message->task = running;
    // No message to be passed.
    struct queue_entry *node = queue_new_node(message);
    pthread_mutex_lock(&i_lock);
    queue_insert_tail(&io_to, node);
    pthread_mutex_unlock(&i_lock);
}
char *sut_read(){
	return NULL;
}

void sut_shutdown(){
    // Shutdown the kernel threads.
	pthread_join(c_exec, NULL);
    pthread_join(i_exec, NULL);
	printf("Threading done! Shutting down.\n");
}