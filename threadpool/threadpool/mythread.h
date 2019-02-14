#pragma once
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

typedef void(*JOBCALLBACK)(void *);

typedef struct NJOB {
	struct NJOB *next;
	JOBCALLBACK func;
	void *arg;
}nJob;// 任务

typedef struct NWORKER {

	struct NWORKER *active_next;
	pthread_t active_tid;
}nWorker;// 活跃的线程 

typedef struct NTHREADPOOL {
	pthread_mutex_t mtx;	// 同步锁

	pthread_cond_t busycv;	// 用于线程池的销毁 
	pthread_cond_t workcv;	// 用于唤醒等待的线程
	pthread_cond_t waitcv;	// 用于线程池空闲等待

	nWorker *active;		// 活跃的线程队列
	nJob *head;				// 任务队列头
	nJob *tail;				// 任务队列尾

	pthread_attr_t attr;	// 线程属性

	int flag;				// 标志当前命令状态 NTY_POOL_WAIT 0X01 NTY_POOL_DESTROY 0X02
	unsigned int wait_time;	// 线程等待任务到来超时时间 超时则说明空闲 退出该线程
	int maxnum;				// 设置线程池中线程最大数
	int minnum;				// 设置线程池中线程最小数
	int nthreads;			// 当前线程池中已有线程数
	int idle;				// 当前线程池中空闲线程数

}nThreadPool;

#define NTY_POOL_WAIT		0x01
#define NTY_POOL_DESTROY	0x02

static pthread_mutex_t nty_pool_lock = PTHREAD_MUTEX_INITIALLIZER; // 全局锁 用于线程销毁时同步
static sigset_t fillset;// 信号集
nThreadPool *thread_pool = NULL;// 线程池句柄

