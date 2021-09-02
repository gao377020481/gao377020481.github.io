


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <fcntl.h>



#define MP_ALIGNMENT       		32  //对齐信息
#define MP_PAGE_SIZE			4096 //单次分配大块大小
#define MP_MAX_ALLOC_FROM_POOL	(MP_PAGE_SIZE-1) 

#define mp_align(n, alignment) (((n)+(alignment-1)) & ~(alignment-1))
#define mp_align_ptr(p, alignment) (void *)((((size_t)p)+(alignment-1)) & ~(alignment-1))





struct mp_large_s {
	struct mp_large_s *next;
	void *alloc;
}; // 当单次分配超过pagesize时就需要一次分配然后归入large的一个链表中保存

struct mp_node_s {

	unsigned char *last;
	unsigned char *end;
	
	struct mp_node_s *next;
	size_t failed;
};// 页内的小区域，用于小块的分配

struct mp_pool_s {

	size_t max;

	struct mp_node_s *current;
	struct mp_large_s *large;

	struct mp_node_s head[0];

}; //内存池

struct mp_pool_s *mp_create_pool(size_t size);
void mp_destory_pool(struct mp_pool_s *pool);
void *mp_alloc(struct mp_pool_s *pool, size_t size);
void *mp_nalloc(struct mp_pool_s *pool, size_t size);
void *mp_calloc(struct mp_pool_s *pool, size_t size);
void mp_free(struct mp_pool_s *pool, void *p);



//首先需要明确，在分配的时候需要将所有的数据结构都存在我们管理的内存池中
//比如struct mp_pool_s *pool这个内存池本身也需要受我们管理
struct mp_pool_s *mp_create_pool(size_t size) {

	struct mp_pool_s *p;
	int ret = posix_memalign((void **)&p, MP_ALIGNMENT, size + sizeof(struct mp_pool_s) + sizeof(struct mp_node_s));
	//posix_memalign 分配足够的内存，size（page_size：4096） 加上内存池本身和小块结构本身
	if (ret) {
		return NULL;
	}
	
	p->max = (size < MP_MAX_ALLOC_FROM_POOL) ? size : MP_MAX_ALLOC_FROM_POOL; //内存池单块大小受我们定义的pagesize限制
	p->current = p->head;// 初始化时head无值且struct中head在最末尾，c++的struct内存布局决定了current就是head
	p->large = NULL;// 还没有分配large块

	p->head->last = (unsigned char *)p + sizeof(struct mp_pool_s) + sizeof(struct mp_node_s); //最后一个分配的小块
	p->head->end = p->head->last + size;//这次分配的页面的末尾

	p->head->failed = 0;

	return p;

}

void mp_destory_pool(struct mp_pool_s *pool) { //释放内存池

	struct mp_node_s *h, *n;
	struct mp_large_s *l;

	for (l = pool->large; l; l = l->next) { //循环释放分配的large块
		if (l->alloc) {
			free(l->alloc);
		}
	}

	h = pool->head->next;

	while (h) { //循环释放分配的页面
		n = h->next;
		free(h);
		h = n;
	}

	free(pool);//释放内存池本身

}

void mp_reset_pool(struct mp_pool_s *pool) { //重置内存池

	struct mp_node_s *h;
	struct mp_large_s *l;

	for (l = pool->large; l; l = l->next) {//重置的时候需要释放大块内存
		if (l->alloc) {
			free(l->alloc);
		}
	}

	pool->large = NULL;

	for (h = pool->head; h; h = h->next) {// 但针对页面只需要将页面内的小块内存指针退回起始位置就可以，不需要将已经分配的页面还给操作系统
		h->last = (unsigned char *)h + sizeof(struct mp_node_s);
	}

}

static void *mp_alloc_block(struct mp_pool_s *pool, size_t size) { // 开辟新的页面并分配内存

	unsigned char *m;
	struct mp_node_s *h = pool->head; //先拿到当前页面的head指针
	size_t psize = (size_t)(h->end - (unsigned char *)h); //拿到页面总大小
	
	int ret = posix_memalign((void **)&m, MP_ALIGNMENT, psize);//分配新的页面
	if (ret) return NULL;

	struct mp_node_s *p, *new_node, *current;
	new_node = (struct mp_node_s*)m; //初始化新的页面

	new_node->end = m + psize;
	new_node->next = NULL;
	new_node->failed = 0;

	m += sizeof(struct mp_node_s); 
	m = mp_align_ptr(m, MP_ALIGNMENT); 
	new_node->last = m + size; //把需要的内存大小从新分配的页面上取走

	current = pool->current;  //关键点：旨在减少页面末尾的内存碎片，nginx使用的方式

	for (p = current; p->next; p = p->next) {  //每一个页面都有一个自己的failed关键字，用于表明其页面末尾提供给新需求时失败的次数，失败次数大于4就将内存池的current指针换到下一个页面
	// 失败次数少于4那么就不变内存池的current指针，这样在下一个需求到来时还是从当前分配失败的(页面末尾内存不够用)这一页面末尾开始查找，有利于减少末尾内存碎片，4这个值的得出应该是nginx的实验
		if (p->failed++ > 4) {
			current = p->next;
		}
	}
	p->next = new_node;

	pool->current = current ? current : new_node; //这里可以看出，如果刚好所有之前创建的页面都失败大于4次，那么将当前内存池首选页面变为刚新建的页面即可

	return m;

}

static void *mp_alloc_large(struct mp_pool_s *pool, size_t size) { //分配大块空间

	void *p = malloc(size);
	if (p == NULL) return NULL;

	size_t n = 0;
	struct mp_large_s *large;
	for (large = pool->large; large; large = large->next) {
		if (large->alloc == NULL) {
			large->alloc = p;
			return p;
		}
		if (n ++ > 3) break;
	}

	large = mp_alloc(pool, sizeof(struct mp_large_s));// large这个数据结构本身也需要交由内存池来管理，分析一下在mp_alloc中因为这个数据结构很小会存储在页面上，故不会产生无限循环
	if (large == NULL) {
		free(p);
		return NULL;
	}

	large->alloc = p;
	large->next = pool->large;
	pool->large = large; //large链表的头插法，很简单

	return p;
}

void *mp_memalign(struct mp_pool_s *pool, size_t size, size_t alignment) {

	void *p;
	
	int ret = posix_memalign(&p, alignment, size);
	if (ret) {
		return NULL;
	}

	struct mp_large_s *large = mp_alloc(pool, sizeof(struct mp_large_s));
	if (large == NULL) {
		free(p);
		return NULL;
	}

	large->alloc = p;
	large->next = pool->large;
	pool->large = large;

	return p;
}




void *mp_alloc(struct mp_pool_s *pool, size_t size) { //内存分配的入口函数，分别处理大块内存和小块内存需求，小块内存需求在页面末尾空间不足时进入新建页面并分配函数中，否则直接分配在当前页面就可以

	unsigned char *m;
	struct mp_node_s *p;

	if (size <= pool->max) {

		p = pool->current;

		do {
			
			m = mp_align_ptr(p->last, MP_ALIGNMENT);
			if ((size_t)(p->end - m) >= size) {
				p->last = m + size;
				return m;
			}
			p = p->next;
		} while (p); //循环在current及其后的一个或多个页面上查找符合要求的末尾空间，存在的话就return

		return mp_alloc_block(pool, size); //进到这里说明不存在符合要求空间，那就新建页面然后分配并对页面failed值计数和调整current页面
	}

	return mp_alloc_large(pool, size); //大块内存情况
	
}


void *mp_nalloc(struct mp_pool_s *pool, size_t size) {

	unsigned char *m;
	struct mp_node_s *p;

	if (size <= pool->max) {
		p = pool->current;

		do {
			m = p->last;
			if ((size_t)(p->end - m) >= size) {
				p->last = m+size;
				return m;
			}
			p = p->next;
		} while (p);

		return mp_alloc_block(pool, size);
	}

	return mp_alloc_large(pool, size);
	
}

void *mp_calloc(struct mp_pool_s *pool, size_t size) {

	void *p = mp_alloc(pool, size);
	if (p) {
		memset(p, 0, size);
	}

	return p;
	
}

void mp_free(struct mp_pool_s *pool, void *p) {

	struct mp_large_s *l;
	for (l = pool->large; l; l = l->next) {
		if (p == l->alloc) {
			free(l->alloc);
			l->alloc = NULL;

			return ;
		}
	}
	
}


int main(int argc, char *argv[]) {

	int size = 1 << 12;

	struct mp_pool_s *p = mp_create_pool(size);

	int i = 0;
	for (i = 0;i < 10;i ++) {

		void *mp = mp_alloc(p, 512);
//		mp_free(mp);
	}

	//printf("mp_create_pool: %ld\n", p->max);
	printf("mp_align(123, 32): %d, mp_align(17, 32): %d\n", mp_align(24, 32), mp_align(17, 32));
	//printf("mp_align_ptr(p->current, 32): %lx, p->current: %lx, mp_align(p->large, 32): %lx, p->large: %lx\n", mp_align_ptr(p->current, 32), p->current, mp_align_ptr(p->large, 32), p->large);

	int j = 0;
	for (i = 0;i < 5;i ++) {

		char *pp = mp_calloc(p, 32);
		for (j = 0;j < 32;j ++) {
			if (pp[j]) {
				printf("calloc wrong\n");
			}
			printf("calloc success\n");
		}
	}

	//printf("mp_reset_pool\n");

	for (i = 0;i < 5;i ++) {
		void *l = mp_alloc(p, 8192);
		mp_free(p, l);
	}

	mp_reset_pool(p);

	//printf("mp_destory_pool\n");
	for (i = 0;i < 58;i ++) {
		mp_alloc(p, 256);
	}

	mp_destory_pool(p);

	return 0;

}



