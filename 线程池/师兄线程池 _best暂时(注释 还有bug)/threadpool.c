#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
 
typedef void (*JOB_CALLBACK)(void *);
 
typedef struct NJOB {
 struct NJOB *next;
 JOB_CALLBACK func;
 void *arg;
} nJob; //任务
 
typedef struct NWORKER {
 struct NWORKER *active_next;
 pthread_t active_tid;
} nWorker;//活跃线程
 
typedef struct NTHREADPOOL {
 struct NTHREADPOOL *forw; //可删
 struct NTHREADPOOL *back; //可删
 pthread_mutex_t mtx;  //同步
  
 //*用于线程池销毁,
 //在nThreadPoolDestroy(pool)中 * 等待 *所有线程销毁
 //在ntyWorkerCleanup(pool) (在线程函数中,退出while(1),以线程清理函数弹栈调用),当线程数为0(所有线程销毁)发出 * 广播 *broadcast
 pthread_cond_t busycv;

 //*用于线程函数中任务等待
 //在ntyWorkerThread 中,任务队列空时* 等待 * 新任务的到来
 //在ntyThreadPoolQueue 中,有空闲线程(正在等待) 发出* 通知(signal) *
 pthread_cond_t workcv;

 //*用于线程池空闲等待(任务队列清空,活跃线程队列清空)
 //在nThreadPoolWait 中,* 等待 *任务队列清空,活跃线程队列清空
 //在ntyNotifyWaiters(pool)中,当任务队列清空,活跃线程队列清空,发出* 广播 *
 pthread_cond_t waitcv;
  
 nWorker *active;		//活跃线程队列
 nJob *head;			//任务队列头
 nJob *tail;			//任务队列尾
 pthread_attr_t attr;	//线程属性
  
 int flags;				//等待 NTY_POOL_WAIT 0X01 NTY_POOL_DESTROY 0X02
 unsigned int linger;	//线程允许等待时长
 int minimum;			//线程池线程数下限
 int maximum;			//线程池线程数上限
 int nthreads;			//线程池当前线程数
 int idle;				//线程池空闲线程数(正在等待的)
  
} nThreadPool;
 
static void* ntyWorkerThread(void *arg);
#define NTY_POOL_WAIT   0x01
#define NTY_POOL_DESTROY  0x02
static int nActives = 0; 
static pthread_mutex_t nty_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static sigset_t fillset;
nThreadPool *thread_pool = NULL;
 


//线程创建   
//在线程函数中退出 线程清理函数调用,当任务队列未清空,线程数未达上限,新增线程增加执行力
//在线程池添加任务函数中 调用(主要:新建线程执行任务)
static int ntyWorkerCreate(nThreadPool *pool) {
 sigset_t oset;
 pthread_t thread_id;
//创建线程时屏蔽信号集
 pthread_sigmask(SIG_SETMASK, &fillset, &oset);
 int error = pthread_create(&thread_id, &pool->attr, ntyWorkerThread, pool);
 pthread_sigmask(SIG_SETMASK, &oset, NULL);
 //创建完屏蔽信号集复原
 return error;
}
 
//线程销毁时机
//1.有多余(超过下限)空闲线程且等待超时
//2.销毁线程池时,销毁活跃线程队列中的线程
//



//线程清理函数 (线程函数中,退出while(1)循环,以线程清理函数调用)
 //因为这是线程清理程序,运行这个程序代表有线程即将销毁了
static void ntyWorkerCleanup(nThreadPool * pool) {
 
 --pool->nthreads;
 printf("nthreads =%d in cleanup\n",pool->nthreads);
 if (pool->flags & NTY_POOL_DESTROY) {
  if (pool->nthreads == 0) {
   pthread_cond_broadcast(&pool->busycv);
  }
 } else if (pool->head != NULL && pool->nthreads < pool->maximum && ntyWorkerCreate(pool) == 0) {
 //任务队列尚未清空,线程数量未达上限,创建新线程,线程数+1,
  pool->nthreads ++;
  printf("heavy load,create thread ,nums = %d\n",pool->nthreads);
 }
 //释放锁
 pthread_mutex_unlock(&pool->mtx);
  
}


//通知线程池不用等待(无任务,无工作线程才通知)
static void ntyNotifyWaiters(nThreadPool *pool) {
	static int cnt = 0;
 if(pool->head == NULL){
	 cnt ++;
	printf("head NULL,notifys = %d\n",cnt);
 }
 if(pool->active == NULL){
	printf("active NULL\n");
 }

 if (pool->head == NULL && pool->active == NULL) {
  pool->flags &= ~NTY_POOL_WAIT;//任务为空,且工作线程为空,取消等待
  printf("broadcast\n");
  pthread_cond_broadcast(&pool->waitcv);//该线程池已经处于空闲,通知不用等待
 }
  
}


//线程任务执行完的任务清理程序   主要是把正在工作的线程指向下一个
static void ntyJobCleanup(nThreadPool *pool) {
 pthread_t tid = pthread_self();
 nWorker *activep;
 nWorker **activepp;
  
 pthread_mutex_lock(&pool->mtx);
 /*
 for (activepp = &pool->active;(activep = *activepp) != NULL;activepp = &activep->active_next) {
  *activepp = activep->active_next;
  nActives--;
  printf("nActives--,cnt = %d \n",nActives);
  break;   //工作线程队列不为空,消除一个,头指针指向下一工作线程
 }
 */
 if(pool->active != NULL){
   nWorker * p = pool->active;
	 pool->active = pool->active->active_next;
	nActives--;
	free(p);  //释放!
	printf("nActives--,cnt = %d \n",nActives);
	if(pool->active == NULL){
		printf("active NULL\n");
	}
 }
 //设置等待标志,如果任务空,工作线程空,则取消等待标记,此时线程池已空闲,通知nThreadPoolWait 不用等待(取消阻塞)
 if (pool->flags & NTY_POOL_WAIT) ntyNotifyWaiters(pool);
}


//线程函数
static void* ntyWorkerThread(void *arg) {
 nThreadPool *pool = (nThreadPool*)arg;
 nWorker* active;
  
 int timeout;			//等待超时标记,1 超时  0 不超时
 struct timespec ts;	//用于设置定时等待 ,传参
 JOB_CALLBACK func;		//任务处理函数
  
 pthread_mutex_lock(&pool->mtx);
 //线程退出清洁函数 压栈,如果中途退出,线程数-1,释放锁
 pthread_cleanup_push(ntyWorkerCleanup, pool);
  
 while (1) {
 //设置线程屏蔽信号集 
  pthread_sigmask(SIG_SETMASK, &fillset, NULL);
 
  //线程设置允许线程取消(pthread_cancel) 且设置pthread_cancel 信号发出后允许线程执行到下一个取消点才结束线程
  pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
  pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
   
  timeout = 0;
  pool->idle ++;
   
  if (pool->flags & NTY_POOL_WAIT) {
   ntyNotifyWaiters(pool);
  }
  

  //等待任务有任务到来 或者 线程池销毁通知
  while (pool->head == NULL && !(pool->flags & NTY_POOL_DESTROY)) {
   if (pool->nthreads <= pool->minimum) {// bug !!!
	   printf("wait infinite unless new task or cmd destroy\n");

     //线程数少于下限,  (线程数量少的就可以无限等待)
    pthread_cond_wait(&pool->workcv, &pool->mtx);
     
   } else {//线程数高于下限,(线程数量高于下限,限时等待)
    
	//限时等待写法,获得绝对时间,+linger(允许等待时长)作为等待定时
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += pool->linger;

    if (pool->linger == 0 || pthread_cond_timedwait(&pool->workcv, &pool->mtx, &ts) == ETIMEDOUT) {
     
     timeout = 1;
     break;
    }
   }
  }
  //等待到条件,处于工作状态,首先空闲线程数-1
  pool->idle --;
  
  //收到销毁通知,退出线程
  if (pool->flags & NTY_POOL_DESTROY){
	static int destroy = 0;
	destroy ++;
	printf("destroy = %d\n",destroy);
	  break;
  }
   
  nJob *job = pool->head;  
  if (job != NULL) {//有任务 消除超时标记
   
   timeout = 0;
   func = job->func;
    
   void *job_arg = job->arg;
   pool->head = job->next;
    
   if (job == pool->tail) {
    pool->tail == NULL;
   }
   //  bug !!!  ??
   active = (nWorker*)malloc(sizeof(nWorker));
   active->active_tid = pthread_self();
   if(pool->active != NULL){
	 active->active_next = pool->active;
	 pool->active = active;
   }
   else{
	 pool->active = active;	
	}
   
	nActives ++;
	printf("nActives ++ ,cnt = %d\n",nActives);
   pthread_mutex_unlock(&pool->mtx);
   //在准备执行线程任务时,防止线程任务退出线程,先添加线程清理程序,防止资源泄露
   pthread_cleanup_push(ntyJobCleanup, pool);
    
   free(job);
   func(job_arg);

   //任务运行完,执行任务清理程序:使下一个工作线程变为活跃线程
   pthread_cleanup_pop(1);
  }
  //有较多(数量大于下限) 的线程,等待超时且一直没有任务到来,退出线程已减少工作线程
  if (timeout && (pool->nthreads > pool->minimum)) {
   break;
  }
 }
 pthread_cleanup_pop(1);//弹栈线程清理程序
 static int quit = 0;
 quit ++;
 if(timeout)
	 printf("useless and timeoout quit cnt  = %d,thread id =%lu \n",quit,pthread_self());
 else
	 printf("cancel quit cnt = %d,thead is = %lu,actives = %d\n",quit,pthread_self(),nActives);
 //线程数 好像没有修改??  在ntyWorkerCleanup 中修改
 return NULL;
  
}


//线程属性拷贝 
static void ntyCloneAttributes(pthread_attr_t *new_attr, pthread_attr_t *old_attr) {
 
 struct sched_param param;
 void *addr;
 size_t size;
 int value;
  
 pthread_attr_init(new_attr);
  
 if (old_attr != NULL) {
  
  pthread_attr_getstack(old_attr, &addr, &size);	//获取线程栈空间
  pthread_attr_setstack(new_attr, NULL, size);		//设置线程栈空间
   
  
  pthread_attr_getscope(old_attr, &value);			//获取线程作用域
  //指定了作用域也就是指定了线程与谁竞争
  pthread_attr_setscope(new_attr, value);			//设置线程作用域
   
  pthread_attr_getinheritsched(old_attr, &value);	//获取线程是否继承调度属性
  pthread_attr_setinheritsched(new_attr, value);	//设置线程是否继承调度属性

  pthread_attr_getschedpolicy(old_attr, &value);	//获取线程的调度策略
  pthread_attr_setschedpolicy(new_attr, value);		//设置线程的调度策略
   
  pthread_attr_getschedparam(old_attr, &param);		//获取线程的调度参数
  pthread_attr_setschedparam(new_attr, &param);		//设置线程的调度参数
   
  pthread_attr_getguardsize(old_attr, &size);		//获取线程保护区大小
  pthread_attr_setguardsize(new_attr, size);		//设置线程保护区大小
   
 }
 //设置线程分离
 pthread_attr_setdetachstate(new_attr, PTHREAD_CREATE_DETACHED);
  
}


// 创建线程池
nThreadPool *ntyThreadPoolCreate(int min_threads, int max_threads, int linger, pthread_attr_t *attr) {
 
 sigfillset(&fillset);
 if (min_threads > max_threads || max_threads < 1) {
  errno = EINVAL;   //表示参数无效  EINVAL 22
  return NULL;
 }
  
 nThreadPool *pool = (nThreadPool*)malloc(sizeof(nThreadPool));
 if (pool == NULL) {
  errno = ENOMEM;  //表示无内存空间
  return NULL;
}
  
 //初始化互斥量  和 条件变量
 pthread_mutex_init(&pool->mtx, NULL);
 pthread_cond_init(&pool->busycv, NULL);
 pthread_cond_init(&pool->workcv, NULL);
 pthread_cond_init(&pool->waitcv, NULL);
  
 pool->active = NULL;
 pool->head = NULL;
 pool->tail = NULL;
 pool->flags = 0;
 pool->linger = linger;
 pool->minimum = min_threads;
 pool->maximum = max_threads;
 pool->nthreads = 0;
 pool->idle = 0;
  
 ntyCloneAttributes(&pool->attr, attr);  //拷贝线程属性
 
 pthread_mutex_lock(&nty_pool_lock);
 

 //可删
 if (thread_pool == NULL) {
  pool->forw = pool;
  pool->back = pool;
   
  thread_pool = pool; //全局变量thread_pool ,加入线程池队列
   
 } else {
  
//back = next   forw = pre  理解
  thread_pool->back->forw = pool;
  pool->forw = thread_pool;
  pool->back = pool->back;//???pool->back = thread_pool->back;  bug? 
  thread_pool->back = pool;
   
 }
  //以上可删
 pthread_mutex_unlock(&nty_pool_lock);
 return pool;
  
}


//压栈任务入线程池
int ntyThreadPoolQueue(nThreadPool *pool, JOB_CALLBACK func, void *arg) {
 
 nJob *job = (nJob*)malloc(sizeof(nJob));
 if (job == NULL) {
  errno = ENOMEM;
  return -1;
 }
 job->next = NULL;
 job->func = func;
 job->arg = arg;
  
 pthread_mutex_lock(&pool->mtx);
 //任务队列(尾)插入新任务
 if (pool->head == NULL) {
  pool->head = job;
 } else {
  pool->tail->next = job;
 }
 pool->tail = job;
//有空闲等待的 唤醒工作
 if (pool->idle > 0) {
  pthread_cond_signal(&pool->workcv);
 } else if (pool->nthreads < pool->maximum && ntyWorkerCreate(pool) == 0) {
  pool->nthreads ++; 
//没有空闲线程,任务队列尚未清空,且线程数未达上限,新建线程去执行,增加执行力
  printf("create threads,nums = %d\n",pool->nthreads);
 }
  
 pthread_mutex_unlock(&pool->mtx);
}


//线程池等待
void nThreadPoolWait(nThreadPool *pool) {
 
 pthread_mutex_lock(&pool->mtx);
//当线程退出时自动执行 解锁  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 pthread_cleanup_push(pthread_mutex_unlock, &pool->mtx);
 
 //有任务,有工作线程,不断设置标记等待, 等待条件
 while (pool->head != NULL || pool->active != NULL) {
  pool->flags |= NTY_POOL_WAIT;
  pthread_cond_wait(&pool->waitcv, &pool->mtx);
 }
  
 pthread_cleanup_pop(1);
}



//线程池销毁
void nThreadPoolDestroy(nThreadPool *pool) {
 
 nWorker *activep;
 nJob *job;
  
 pthread_mutex_lock(&pool->mtx);
 //设置线程清理函数,退出自动解锁
 pthread_cleanup_push(pthread_mutex_unlock, &pool->mtx);

 //设置销毁标记,群发通知,唤醒所有正在等待的线程,
 //若之前调用nthreadpoolwait,此时应该只剩下限个线程,且任务队列为空,活跃线程队列为空
 pool->flags |= NTY_POOL_DESTROY;
 pthread_cond_broadcast(&pool->workcv);
 
 //向线程池所有正在工作的线程 发出线程取消通知
 for (activep = pool->active;activep != NULL;activep = activep->active_next) {
  pthread_cancel(activep->active_tid);
 }
 
 //线程数不为0,等待,线程池处于忙状态
 while (pool->nthreads != 0) {
  pthread_cond_wait(&pool->busycv, &pool->mtx);
 }
  
 pthread_cleanup_pop(1);//线程清理:  解锁

 pthread_mutex_lock(&nty_pool_lock);
  
 if (thread_pool == pool) {  //这3行bug 吗??  多余?
  thread_pool = pool->forw;
 }
  
 if (thread_pool == pool) {//????
  thread_pool = NULL;
 } else {
  pool->back->forw = pool->forw;     //不要判断指针为空吗?
  pool->forw->back = pool->back;
 }
  
 pthread_mutex_unlock(&nty_pool_lock);
  
 //工作队列,从头开始 销毁
 for (job = pool->head;job != NULL;job = pool->head) {
  pool->head = job->next;
  free(job);
 }
 pthread_attr_destroy(&pool->attr);
 free(pool);
  
}
 
/********************************* debug thread pool *********************************/
 
void king_counter(void *arg) {
 int index = *(int*)arg;
 //printf("index : %d, selfid : %lu\n", index, pthread_self());
  
 free(arg);
 usleep(500);
//  sleep(1);
}
 
 
#define KING_COUNTER_SIZE 20
 
 
int main(int argc, char *argv[]) {
//线程数下限,上限,线程允许等待时长,线程屏蔽信号集,创建线程池 
 nThreadPool *pool = ntyThreadPoolCreate(5, 10, 3, NULL);
  
 int i = 0;
 for (i = 0;i < KING_COUNTER_SIZE;i ++) {
  
  int *index = (int*)malloc(sizeof(int));
   
  memset(index, 0, sizeof(int));
  memcpy(index, &i, sizeof(int));
   
  ntyThreadPoolQueue(pool, king_counter, index);
   
 }
 printf("waiting\n");
 nThreadPoolWait(pool);

 
 sleep(4);
 printf("destroy pool\n");
 nThreadPoolDestroy(pool);

  
  
 getchar();
 printf("You are very good !!!!\n");
}
