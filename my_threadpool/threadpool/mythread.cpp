#include "mythread.h"

// 线程创建
static int WorkerCreate(nThreadPool *pool)
{
	sigset_t oset;
	pthread_t thread_id;

	// 创建线程时屏蔽信号集  防止被信号打断
	pthread_sigmask(SIG_SETMASK, &fillset, &oset);
	int error = pthread_create(&thread_id, &pool->attr, WorkerThread, pool);
	// 恢复原始屏蔽集
	pthread_sigmask(SIG_SETMASK, &oset, NULL);
	return error;
}

// 线程池属性拷贝
static void ntyCloneAttribute(pthread_attr_t *new_attr, pthread_attr_t *old_attr)
{
	struct sched_param param; // 优先级参数
	void *addr;
	size_t size;
	int value;

	pthread_attr_init(new_attr);

	// 将old_attr 中的属性拷贝进new_attr
	if (old_attr != NULL)
	{
		pthread_attr_getstack(old_attr, &addr, &size); // 获取线程栈空间
		pthread_attr_setstack(new_attr, NULL, size);

		pthread_attr_getscope(old_attr, &value);// 设置线程作用域 指定作用域也就是指定线程与谁竞争
		pthread_attr_setscope(new_attr, value);

		pthread_attr_getinheritsched(old_attr, &value);
		pthread_attr_setinheritsched(new_attr, value);// 设置线程是否继承调度属性

		pthread_attr_getschedpolicy(old_attr, &value);// 设置线程的调度策略属性
		pthread_attr_setschedpolicy(new_attr, value);

		pthread_attr_getschedparam(old_attr, &param);// 设置线程的调度参数
		pthread_attr_getschedparam(new_attr, &param);

		pthread_attr_getguardsize(old_attr, &size); // 设置线程保护区大小
		pthread_attr_setguardsize(new_attr, size);
	}

	// 设置线程为分离属性
	pthread_attr_setdetachstate(new_attr, PTHREAD_CREATE_DETACHED);

}

// 创建线程池
nThreadPool *ThreadPoolCreate(int min_threads, int max_threads, int wait_time, pthread_attr_t *attr)
{
	sigfillset(&fillset);  // 用来将参数set信号集初始化，然后把所有的信号加入到此信号集里
	// 非法参数过滤
	if (min_threads > max_threads || max_threads < 1) {
		errno = EINVAL;
		return NULL;
	}

	nThreadPool *pool = (nThreadPool*)malloc(sizeof(nThreadPool));

	// 内存分配异常处理
	if (pool == NULL)
	{
		errno = ENOMEM;
		return NULL;
	}

	// 初始化锁 和 条件变量
	pthread_mutex_init(&pool->mtx, NULL);
	pthread_cond_init(&pool->busycv, NULL);
	pthread_cond_init(&pool->workcv, NULL);
	pthread_cond_init(&pool->waitcv, NULL);

	pool->active = NULL;
	pool->head = NULL;
	pool->tail = NULL;
	pool->flag = 0;
	pool->wait_time = wait_time;
	pool->minnum = min_threads;
	pool->maxnum = max_threads;
	pool->nthreads = 0;
	pool->idle = 0;
	
	ntyCloneAttribute(&pool->attr, attr); // 设置线程属性

	return pool;
}

// 添加任务进线程池
int ThreadPoolQueue(nThreadPool *pool, JOBCALLBACK func, void *arg)
{
	nJob *job = (nJob*)malloc(sizeof(nJob));
	
	if (job == NULL)
	{
		errno = ENOMEM;
		return -1;
	}
	job->next = NULL;
	job->func = func;
	job->arg = arg;

	pthread_mutex_lock(&pool->mtx);

	// 将任务加入任务队列队尾
	if (pool->head == NULL)
		pool->head = job;
	else
	{
		pool->tail->next = job;
	}
	pool->tail = job;

	// 如果有空闲的线程 就不必产生新的线程 直接唤醒就行
	if (pool->idle > 0)
	{
		pthread_cond_signal(&pool->workcv);
	}
	else if (pool->nthreads < pool->maxnum && WorkerCreate(pool) == 0)
	{
		++ pool->nthreads;
	}

	pthread_mutex_unlock(&pool->mtx);
}

static void* WorkerThread(void *arg)
{
	nThreadPool *pool = (nThreadPool*)arg;
	nWorker* active;

	int timeout;			//等待超时标记 1 超时 0 不超时
	struct timespec ts;		// 用于设置定时等待
	JOBCALLBACK func;		// 任务处理函数

	pthread_mutex_lock(&pool->mtx);
	// 线程退出清理函数 压栈，线程数 -1 ，释放锁
	pthread_cleanup_push(WorkerCleanup, pool);

	while (1)
	{
		// 设置线程屏蔽信号集
		pthread_sigmask(SIG_SETMASK, &fillset, NULL);

		// 线程设置允许线程取消（pthread_cancel） 且设置pthread_cancel 信号发出后允许线程执行到下一个取消点才结束线程
		pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
		pthread_setcanceltype(PTHREAD_CANCEL_ENABLE, NULL);

		timeout = 0;
		pool->idle++;
		
		if (pool->flag & NTY_POOL_WAIT) // 如果命令是 wait
		{
			NotifyWaiters(pool);
		}

		// 等待任务 有任务到来或者线程池销毁通知
		while (pool->head == NULL && !(pool->flag & NTY_POOL_DESTROY))
		{
			if (pool->nthreads <= pool->minnum)
			{
				// 线程数少于下限 无条件等待
				pthread_cond_wait(&pool->workcv, &pool->mtx);
			}
			else
			{
				// z线程数高于下限 限时等待
				clock_gettime(CLOCK_REALTIME, &ts);
				ts.tv_sec += pool->wait_time;

				if (pool->wait_time == 0 || pthread_cond_timedwait(&pool->workcv, &pool->mtx, &ts))
				{
					timeout = 1;
					break;
				}
			}

		}
		// 等待到条件，处于工作状态，首先空闲线程数 -1
		pool->idle--;

		// 如果是销毁的通知 退出线程
		if (pool->flag & NTY_POOL_DESTROY) break;

		nJob *job = pool->head;

		if (job != NULL)
		{
			timeout = 0;
			func = job->func;

			void *job_arg = job->arg;
			pool->head = job->next;

			if (job == pool->tail)
			{
				pool->tail = NULL;
			}

			active = (nWorker*)malloc(sizeof(nWorker));
			active->active_tid = pthread_self();
			if (pool->active != NULL)
			{
				active->active_next = pool->active;
				pool->active = active;
			}
			else
			{
				pool->active = active;
			}

			pthread_mutex_unlock(&pool->mtx);

			pthread_cleanup_push(JobCleanup, pool);

			free(job);
			func(job_arg);

			// 任务运行完，执行任务清理程序，使下一个 线程变为活跃线程
			pthread_cleanup_pop(1);
		}
		// 如果任务等待超时，说明空闲，则退出线程
		if (timeout && (pool->nthreads > pool->minnum)) 
			break;

	}
	pthread_cleanup_pop(1); // 弹出线程清理函数
	return NULL;

}

//线程任务执行完的任务清理程序   主要是把正在工作的线程指向下一个
static void JobCleanup(nThreadPool *pool)
{
	pthread_t t_id = pthread_self();

	pthread_mutex_lock(&pool->mtx); // 跟whil(1) 后面的pthread_cond_wait 相对应  等待条件需要加锁

	if (pool->active != NULL)
	{
		nWorker *p = pool->active;
		pool->active = pool->active->active_next;
		free(p);
	}

	// 设置等待标志,如果任务空,工作线程空,则取消等待标记,此时线程池已空闲,通知nThreadPoolWait 不用等待(取消阻塞)
	// if (pool->flags & NTY_POOL_WAIT) ntyNotifyWaiters(pool);
}

static void WorkerCleanup(nThreadPool *pool)
{
	--pool->nthreads;

	if (pool->flag & NTY_POOL_DESTROY)
	{
		if (pool->nthreads == 0)
			pthread_cond_broadcast(&pool->busycv);
	}
	else if (pool->head != NULL && pool->nthreads < pool->maxnum && WorkerCreate(pool) == 0) 
	{
		// 主要是当pthread_cancer 或 pthread_exit 主动调用才考虑这种情况 为了早点执行完任务 多生产线程
		//任务队列尚未清空,线程数量未达上限,创建新线程,线程数+1,
		pool->nthreads++;
	}
	// 否则直接结束当前线程

	// 释放锁  跟线程函数中 while(1) 之前的加锁相对应
}

// 线程池等待
void ThreadPoolWait(nThreadPool* pool)
{
	pthread_mutex_lock(&pool->mtx);

	// 当线程退出时自动执行解锁？？？？？  为什么不直接解锁
	// 区别跟原版不一样
	while (pool->head != NULL || pool->active != NULL)
	{
		pool->flag |= NTY_POOL_WAIT;
		pthread_cond_wait(&pool->waitcv, &pool->mtx);
	}

	pthread_mutex_unlock(&pool->mtx);

}

// 通知线程池不用等待(无任务,无工作线程才通知)
static void NotifyWaiters(nThreadPool *pool)
{
	if (pool->head == NULL && pool->active == NULL)
	{
		pool->flag &= ~NTY_POOL_WAIT;//任务为空,且工作线程为空,取消等待
		pthread_cond_broadcast(&pool->waitcv);//该线程池已经处于空闲,通知不用等待
	}
}

// 线程池销毁
void nThreadPoolDestroy(nThreadPool *pool)
{
	nWorker *activep;
	nJob *job;

	pthread_mutex_lock(&pool->mtx);
	// 设置线程清理函数 退出自动解锁
	pthread_cleanup_push(pthread_mutex_unlock, &pool->mtx);

	// 设置销毁标记，广播通知 唤醒所有正在等待执行任务的线程

	pool->flag |= NTY_POOL_DESTROY;
	pthread_cond_broadcast(&pool->workcv);

	// 向线程池所有正在工作的线程 发出线程取消通知
	for (activep = pool->active; activep != NULL; activep = activep->active_next)
	{
		pthread_cancel(activep->active_tid);
	}

	// 如果经过上一步处理后 线程数还是不为 0  那么就要等待，因为线程池处于忙状态
	while (pool->nthreads != 0)
	{
		pthread_cond_wait(&pool->busycv, &pool->mtx);
	}
	pthread_cleanup_pop(1);// 线程清理解锁

	// 销毁任务队列
	for (job = pool->head; job != NULL; job = pool->head)
	{
		pool->head = job->next;
		free(job);
	}

	pthread_attr_destroy(&pool->attr); // 非要加么？
	free(pool);
}

/*********************debug thread pool*******************************************/

void testfunc(void *arg)
{
	int index = *(int *)arg;
	printf("index : %d,selfid : %lu\n", index, pthread_self());

	free(arg);
	usleep(1);
}

#define COUNT_SIZE 1000

int main(int argc, char *arg[])
{
	nThreadPool *pool = ThreadPoolCreate(10, 20, 15, NULL);

	int i = 0;
	for (i = 0; i < COUNT_SIZE; ++i)
	{
		int *index = (int*)malloc(sizeof(int));

		memset(index, 0, sizeof(int));
		memcpy(index, &i, sizeof(int));

		ThreadPoolQueue(pool, testfunc, index);
	}

	getchar();
	printf("Nice!!!!\n");
}