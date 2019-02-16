
#ifndef _ASYNC_QUEUE_INTERLER_H_
#define _ASYNC_QUEUE_INTERLER_H_

#include "queue.h"

#include <pthread.h>

typedef struct async_queue
{
    pthread_mutex_t  mutex;
    pthread_cond_t   cond;
    int              waiting_threads;

    queue_t*         queue;
    int              quit;   // 0 ��ʾ���˳�  1 ��ʾ�˳�

    int              efd;     //event fd,
    int              epollfd; // epoll fd

    /* ���Ա��� */
    long long        tasked;  // �Ѿ����������������
} async_queue_t;


typedef struct async_queue_op
{
    const char* name; // such as cond, eventfd, lock free and so on.
    async_queue_t* (*create)(int size);
    BOOL  (*push)(async_queue_t* queue, task_t* t);
    task_t* (*pop)(async_queue_t* queue, int timeout);
    void  (*free)(async_queue_t* q);
    BOOL  (*empty)(async_queue_t* q);
    BOOL  (*destory)(async_queue_t* q);
}async_queue_op_t;

#endif

