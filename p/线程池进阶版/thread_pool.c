

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>



#define LL_ADD(item, list) do { 	\
	item->prev = NULL;				\
	item->next = list;				\
	list = item;					\
} while(0)

#define LL_REMOVE(item, list) do {						\
	if (item->prev != NULL) item->prev->next = item->next;	\
	if (item->next != NULL) item->next->prev = item->prev;	\
	if (list == item) list = item->next;					\
	item->prev = item->next = NULL;							\
} while(0)


typedef struct NWORKER {//工作线程信息
	pthread_t thread; //线程id
	int terminate; //是否要终止
	struct NWORKQUEUE *workqueue; //线程池，用于找到工作队列
	struct NWORKER *prev;
	struct NWORKER *next;
} nWorker;

typedef struct NJOB {  //工作个体
	void (*job_function)(struct NJOB *job); 
	void *user_data;
	struct NJOB *prev;
	struct NJOB *next;
} nJob;

typedef struct NWORKQUEUE {
	struct NWORKER *workers; //所有工作线程的链表
	struct NJOB *waiting_jobs; //工作队列
	pthread_mutex_t jobs_mtx; 
	pthread_cond_t jobs_cond;
} nWorkQueue;

typedef nWorkQueue nThreadPool;

static void *ntyWorkerThread(void *ptr) { //工作线程取用工作
	nWorker *worker = (nWorker*)ptr;

	while (1) {
		pthread_mutex_lock(&worker->workqueue->jobs_mtx); //先获取工作队列的操作互斥锁

		while (worker->workqueue->waiting_jobs == NULL) {
			if (worker->terminate) break;
			pthread_cond_wait(&worker->workqueue->jobs_cond, &worker->workqueue->jobs_mtx); //如果工作队列为空，这个线程就阻塞在条件变量上等待事件发生
		}

		if (worker->terminate) {
			pthread_mutex_unlock(&worker->workqueue->jobs_mtx); //如果检测到工作线程被终止，那么这个线程就需要结束工作，但在结束工作前需要将对工作队列的取用权限放开，所以这里在break前需要解锁这个互斥锁
			break;
		}
		
		nJob *job = worker->workqueue->waiting_jobs; //从工作队列中获取一个工作
		if (job != NULL) {
			LL_REMOVE(job, worker->workqueue->waiting_jobs);  //从工作队列中移除掉获取的这个工作
		if (job != NULL) {
		}
		
		pthread_mutex_unlock(&worker->workqueue->jobs_mtx); //已经取到工作了，就可以放开对工作队列的占有了

		if (job == NULL) continue;

		job->job_function(job); //针对工作调用他的回调函数处理，处理结束后继续循环去工作队列中取
	}

	free(worker); //工作线程被终止那当然需要释放其堆上内存
	pthread_exit(NULL);
}



int ntyThreadPoolCreate(nThreadPool *workqueue, int numWorkers) { //创建线程池

	if (numWorkers < 1) numWorkers = 1;
	memset(workqueue, 0, sizeof(nThreadPool));
	
	pthread_cond_t blank_cond = PTHREAD_COND_INITIALIZER;  //条件变量用于通知所有的工作线程事件发生
	memcpy(&workqueue->jobs_cond, &blank_cond, sizeof(workqueue->jobs_cond));
	
	pthread_mutex_t blank_mutex = PTHREAD_MUTEX_INITIALIZER; // 互斥锁用于锁住工作队列确保同时只有一个工作线程在从工作队列中取工作
	memcpy(&workqueue->jobs_mtx, &blank_mutex, sizeof(workqueue->jobs_mtx));

	int i = 0;
	for (i = 0;i < numWorkers;i ++) {
		nWorker *worker = (nWorker*)malloc(sizeof(nWorker)); //这里在堆上创建线程，那就需要在线程终止时释放
		if (worker == NULL) {
			perror("malloc");
			return 1;
		}

		memset(worker, 0, sizeof(nWorker));
		worker->workqueue = workqueue;  //初始化工作线程信息，线程池用于找到工作队列

		int ret = pthread_create(&worker->thread, NULL, ntyWorkerThread, (void *)worker); //创建线程，传入该线程信息，达到信息和线程的绑定关系
		if (ret) {
			
			perror("pthread_create");
			free(worker);

			return 1;
		}

		LL_ADD(worker, worker->workqueue->workers); //头插法在所有工作线程的链表中插入新建的工作线程
	}

	return 0;
}


void ntyThreadPoolShutdown(nThreadPool *workqueue) { //关闭线程池
	nWorker *worker = NULL;

	for (worker = workqueue->workers;worker != NULL;worker = worker->next) {
		worker->terminate = 1;    //所有工作线程的terminate关键字置为1
	}

	pthread_mutex_lock(&workqueue->jobs_mtx);

	workqueue->workers = NULL;  //清空工作线程链表
	workqueue->waiting_jobs = NULL; // 清空工作队列

	pthread_cond_broadcast(&workqueue->jobs_cond); //告诉所有工作线程有事件发生(shutdown，下一步检查terminate关键字)

	pthread_mutex_unlock(&workqueue->jobs_mtx);
	
}

void ntyThreadPoolQueue(nThreadPool *workqueue, nJob *job) { //向工作队列中添加工作

	pthread_mutex_lock(&workqueue->jobs_mtx);

	LL_ADD(job, workqueue->waiting_jobs);
	
	pthread_cond_signal(&workqueue->jobs_cond); //告诉任意一个工作线程有事件发生(目前有新的工作出现在工作队列里了，下一步get互斥锁并取工作)
	pthread_mutex_unlock(&workqueue->jobs_mtx);
	
}




/************************** debug thread pool **************************/
//sdk  --> software develop kit
// 提供SDK给其他开发者使用

#if 1

#define KING_MAX_THREAD			80
#define KING_COUNTER_SIZE		1000

void king_counter(nJob *job) {

	int index = *(int*)job->user_data;

	printf("index : %d, selfid : %lu\n", index, pthread_self());
	
	free(job->user_data);
	free(job);
}



int main(int argc, char *argv[]) {

	nThreadPool pool;

	ntyThreadPoolCreate(&pool, KING_MAX_THREAD);
	
	int i = 0;
	for (i = 0;i < KING_COUNTER_SIZE;i ++) {
		nJob *job = (nJob*)malloc(sizeof(nJob));
		if (job == NULL) {
			perror("malloc");
			exit(1);
		}
		
		job->job_function = king_counter;
		job->user_data = malloc(sizeof(int));
		*(int*)job->user_data = i;

		ntyThreadPoolQueue(&pool, job);
		
	}

	getchar();
	printf("\n");

	
}

#endif
