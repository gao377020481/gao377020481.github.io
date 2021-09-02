


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <errno.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <sys/epoll.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <pthread.h>


#define DNS_SVR				"114.114.114.114"


#define DNS_HOST			0x01
#define DNS_CNAME			0x05

#define ASYNC_CLIENT_NUM		1024


struct dns_header { //dns 头
	unsigned short id;
	unsigned short flags;
	unsigned short qdcount;
	unsigned short ancount;
	unsigned short nscount;
	unsigned short arcount;
};

struct dns_question {//dns 请求
	int length;
	unsigned short qtype;
	unsigned short qclass;
	char *qname;
};

struct dns_item {//dns 基本信息
	char *domain;
	char *ip;
};

typedef void (*async_result_cb)(struct dns_item *list, int count); //请求端针对返回数据的处理函数（以回调函数的方式存在，在recv后调用，通过epoll中的event内的data中ptr传递，这一点与reactor中很像）


struct async_context { //异步请求池中 epoll_wait单独占用一个线程 且epoll也需要在其他线程中出现，这一部分为不同线程共用部分，所以命名为async context也叫上下文
//上下文中一般来讲还需要保存进行epoll_wait的线程id，但这里保存也没啥用
	int epfd;
};

struct ep_arg {  //每一个请求（fd）和其处理函数(cb)存在一个sturct中，与reactor 类似 
	int sockfd;
	async_result_cb cb;
};

int dns_create_header(struct dns_header *header) { //dns 创建头

	if (header == NULL) return -1;
	memset(header, 0, sizeof(struct dns_header));

	srandom(time(NULL));

	header->id = random();
	header->flags |= htons(0x0100);
	header->qdcount = htons(1);

	return 0;
}

int dns_create_question(struct dns_question *question, const char *hostname) { //dns 创建请求

	if (question == NULL) return -1;
	memset(question, 0, sizeof(struct dns_question));

	question->qname = (char*)malloc(strlen(hostname) + 2);
	if (question->qname == NULL) return -2;

	question->length = strlen(hostname) + 2;

	question->qtype = htons(1);
	question->qclass = htons(1);

	const char delim[2] = ".";

	char *hostname_dup = strdup(hostname);
	char *token = strtok(hostname_dup, delim);

	char *qname_p = question->qname;

	while (token != NULL) {

		size_t len = strlen(token);

		*qname_p = len;
		qname_p ++;

		strncpy(qname_p, token, len+1);
		qname_p += len;

		token = strtok(NULL, delim);
	}

	free(hostname_dup);

	return 0;
	
}

int dns_build_request(struct dns_header *header, struct dns_question *question, char *request) {//dns 创建请求

	int header_s = sizeof(struct dns_header);
	int question_s = question->length + sizeof(question->qtype) + sizeof(question->qclass);

	int length = question_s + header_s;

	int offset = 0;
	memcpy(request+offset, header, sizeof(struct dns_header));
	offset += sizeof(struct dns_header);

	memcpy(request+offset, question->qname, question->length);
	offset += question->length;

	memcpy(request+offset, &question->qtype, sizeof(question->qtype));
	offset += sizeof(question->qtype);

	memcpy(request+offset, &question->qclass, sizeof(question->qclass));

	return length;
	
}

static int is_pointer(int in) { 
	return ((in & 0xC0) == 0xC0);
}

static int set_block(int fd, int block) { //设置fd的阻塞类型
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0) return flags;

	if (block) {        
		flags &= ~O_NONBLOCK;    
	} else {        
		flags |= O_NONBLOCK;    
	}

	if (fcntl(fd, F_SETFL, flags) < 0) return -1;

	return 0;
}

static void dns_parse_name(unsigned char *chunk, unsigned char *ptr, char *out, int *len) { //dns 解析域名

	int flag = 0, n = 0, alen = 0;
	char *pos = out + (*len);

	while (1) {

		flag = (int)ptr[0];
		if (flag == 0) break;

		if (is_pointer(flag)) {
			
			n = (int)ptr[1];
			ptr = chunk + n;
			dns_parse_name(chunk, ptr, out, len);
			break;
			
		} else {

			ptr ++;
			memcpy(pos, ptr, flag);
			pos += flag;
			ptr += flag;

			*len += flag;
			if ((int)ptr[0] != 0) {
				memcpy(pos, ".", 1);
				pos += 1;
				(*len) += 1;
			}
		}
	
	}
	
}


static int dns_parse_response(char *buffer, struct dns_item **domains) {//dns 解析应答

	int i = 0;
	unsigned char *ptr = buffer;

	ptr += 4;
	int querys = ntohs(*(unsigned short*)ptr);

	ptr += 2;
	int answers = ntohs(*(unsigned short*)ptr);

	ptr += 6;
	for (i = 0;i < querys;i ++) {
		while (1) {
			int flag = (int)ptr[0];
			ptr += (flag + 1);

			if (flag == 0) break;
		}
		ptr += 4;
	}

	char cname[128], aname[128], ip[20], netip[4];
	int len, type, ttl, datalen;

	int cnt = 0;
	struct dns_item *list = (struct dns_item*)calloc(answers, sizeof(struct dns_item));
	if (list == NULL) {
		return -1;
	}

	for (i = 0;i < answers;i ++) {
		
		bzero(aname, sizeof(aname));
		len = 0;

		dns_parse_name(buffer, ptr, aname, &len);
		ptr += 2;

		type = htons(*(unsigned short*)ptr);
		ptr += 4;

		ttl = htons(*(unsigned short*)ptr);
		ptr += 4;

		datalen = ntohs(*(unsigned short*)ptr);
		ptr += 2;

		if (type == DNS_CNAME) {

			bzero(cname, sizeof(cname));
			len = 0;
			dns_parse_name(buffer, ptr, cname, &len);
			ptr += datalen;
			
		} else if (type == DNS_HOST) {

			bzero(ip, sizeof(ip));

			if (datalen == 4) {
				memcpy(netip, ptr, datalen);
				inet_ntop(AF_INET , netip , ip , sizeof(struct sockaddr));

				printf("%s has address %s\n" , aname, ip);
				printf("\tTime to live: %d minutes , %d seconds\n", ttl / 60, ttl % 60);

				list[cnt].domain = (char *)calloc(strlen(aname) + 1, 1);
				memcpy(list[cnt].domain, aname, strlen(aname));
				
				list[cnt].ip = (char *)calloc(strlen(ip) + 1, 1);
				memcpy(list[cnt].ip, ip, strlen(ip));
				
				cnt ++;
			}
			
			ptr += datalen;
		}
	}

	*domains = list;
	ptr += 2;

	return cnt;
	
}


int dns_client_commit(const char *domain) { //这个是同步请求中的提交，大概看一下就可以

	int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0) {
		perror("create socket failed\n");
		exit(-1);
	}

	printf("url:%s\n", domain);

	set_block(sockfd, 0); //nonblock

	struct sockaddr_in dest;
	bzero(&dest, sizeof(dest));
	dest.sin_family = AF_INET;
	dest.sin_port = htons(53);
	dest.sin_addr.s_addr = inet_addr(DNS_SVR);
	
	int ret = connect(sockfd, (struct sockaddr*)&dest, sizeof(dest));
	//printf("connect :%d\n", ret);

	struct dns_header header = {0};
	dns_create_header(&header);

	struct dns_question question = {0};
	dns_create_question(&question, domain);

	char request[1024] = {0};
	int req_len = dns_build_request(&header, &question, request);
	int slen = sendto(sockfd, request, req_len, 0, (struct sockaddr*)&dest, sizeof(struct sockaddr));

	while (1) {
		char buffer[1024] = {0};
		struct sockaddr_in addr;
		size_t addr_len = sizeof(struct sockaddr_in);
	
		int n = recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr*)&addr, (socklen_t*)&addr_len);
		if (n <= 0) continue;
		
		printf("recvfrom n : %d\n", n);
		struct dns_item *domains = NULL;
		dns_parse_response(buffer, &domains);

		break;
	}

	return 0;
}

void dns_async_client_free_domains(struct dns_item *list, int count) { 
	int i = 0;

	for (i = 0;i < count;i ++) {
		free(list[i].domain);
		free(list[i].ip);
	}

	free(list);
}


//dns_async_client_proc()
//epoll_wait
//result callback
static void* dns_async_client_proc(void *arg) { //异步请求框架第三环节。 callback函数，这个callback函数不是用于处理接受数据的callback，而是initial中用于接受和处理数据的线程运行的函数，在线程创建时传入
	struct async_context *ctx = (struct async_context*)arg;

	int epfd = ctx->epfd;

	while (1) {

		struct epoll_event events[ASYNC_CLIENT_NUM] = {0};

		int nready = epoll_wait(epfd, events, ASYNC_CLIENT_NUM, -1);
		if (nready < 0) {
			if (errno == EINTR || errno == EAGAIN) {
				continue;
			} else {
				break;
			}
		} else if (nready == 0) {
			continue;
		}

		printf("nready:%d\n", nready);
		int i = 0;
		for (i = 0;i < nready;i ++) {

			struct ep_arg *data = (struct ep_arg*)events[i].data.ptr;
			int sockfd = data->sockfd;

			char buffer[1024] = {0};
			struct sockaddr_in addr;
			size_t addr_len = sizeof(struct sockaddr_in);
			int n = recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr*)&addr, (socklen_t*)&addr_len);

			struct dns_item *domain_list = NULL;
			int count = dns_parse_response(buffer, &domain_list);

			data->cb(domain_list, count); //call cb
			
			int ret = epoll_ctl(epfd, EPOLL_CTL_DEL, sockfd, NULL);
			//printf("epoll_ctl DEL --> sockfd:%d\n", sockfd);

			close(sockfd); ///// //异步请求框架第四环节。 关闭

			dns_async_client_free_domains(domain_list, count);
			free(data);

		}
		
	}
	
}



//dns_async_client_init()
//epoll init
//thread init
struct async_context *dns_async_client_init(void) { //异步请求框架第一环节。 initial 初始化上下文信息，主要是创建epoll、创建线程（接受数据的线程）

	int epfd = epoll_create(1); // 
	if (epfd < 0) return NULL;

	struct async_context *ctx = calloc(1, sizeof(struct async_context));
	if (ctx == NULL) {
		close(epfd);
		return NULL;
	}
	ctx->epfd = epfd;

	pthread_t thread_id;
	int ret = pthread_create(&thread_id, NULL, dns_async_client_proc, ctx);
	if (ret) {
		perror("pthread_create");
		return NULL;
	}
	usleep(1); //child go first

	return ctx;
}


//dns_async_client_commit(ctx, domain)
//socket init
//dns_request
//sendto dns send
int dns_async_client_commit(struct async_context* ctx, const char *domain, async_result_cb cb) { //异步请求框架第二环节 commit，提交请求。 可以看到传参上下文，获取epoll，将新来的请求提交并加入epoll中在另一个线程中等待返回数据并使用传递的cb解析

	int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0) {
		perror("create socket failed\n");
		exit(-1);
	}

	printf("url:%s\n", domain);

	set_block(sockfd, 0); //nonblock

	struct sockaddr_in dest;
	bzero(&dest, sizeof(dest));
	dest.sin_family = AF_INET;
	dest.sin_port = htons(53);
	dest.sin_addr.s_addr = inet_addr(DNS_SVR);
	
	int ret = connect(sockfd, (struct sockaddr*)&dest, sizeof(dest));
	//printf("connect :%d\n", ret);

	struct dns_header header = {0};
	dns_create_header(&header);

	struct dns_question question = {0};
	dns_create_question(&question, domain);

	char request[1024] = {0};
	int req_len = dns_build_request(&header, &question, request);
	int slen = sendto(sockfd, request, req_len, 0, (struct sockaddr*)&dest, sizeof(struct sockaddr));

	struct ep_arg *eparg = (struct ep_arg*)calloc(1, sizeof(struct ep_arg)); //初始化这个新请求后续返回结果对应的处理函数，并通过下面ev.data.ptr = eparg保存并通过另一个线程检测到返回数据后调用
	if (eparg == NULL) return -1;
	eparg->sockfd = sockfd;
	eparg->cb = cb;

	struct epoll_event ev;
	ev.data.ptr = eparg;
	ev.events = EPOLLIN;

	ret = epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, sockfd, &ev); 
	//printf(" epoll_ctl ADD: sockfd->%d, ret:%d\n", sockfd, ret);

	return ret;


	
}


char *domain[] = {
	"www.ntytcp.com",
	"bojing.wang",
	"www.baidu.com",
	"tieba.baidu.com",
	"news.baidu.com",
	"zhidao.baidu.com",
	"music.baidu.com",
	"image.baidu.com",
	"v.baidu.com",
	"map.baidu.com",
	"baijiahao.baidu.com",
	"xueshu.baidu.com",
	"cloud.baidu.com",
	"www.163.com",
	"open.163.com",
	"auto.163.com",
	"gov.163.com",
	"money.163.com",
	"sports.163.com",
	"tech.163.com",
	"edu.163.com",
	"www.taobao.com",
	"q.taobao.com",
	"sf.taobao.com",
	"yun.taobao.com",
	"baoxian.taobao.com",
	"www.tmall.com",
	"suning.tmall.com",
	"www.tencent.com",
	"www.qq.com",
	"www.aliyun.com",
	"www.ctrip.com",
	"hotels.ctrip.com",
	"hotels.ctrip.com",
	"vacations.ctrip.com",
	"flights.ctrip.com",
	"trains.ctrip.com",
	"bus.ctrip.com",
	"car.ctrip.com",
	"piao.ctrip.com",
	"tuan.ctrip.com",
	"you.ctrip.com",
	"g.ctrip.com",
	"lipin.ctrip.com",
	"ct.ctrip.com"
};

static void dns_async_client_result_callback(struct dns_item *list, int count) {
	int i = 0;

	for (i = 0;i < count;i ++) {
		printf("name:%s, ip:%s\n", list[i].domain, list[i].ip);
	}
}


int main(int argc, char *argv[]) {
#if 0
	dns_client_commit(argv[1]);
#else

	struct async_context *ctx = dns_async_client_init();
	if (ctx == NULL) return -2;

	int count = sizeof(domain) / sizeof(domain[0]);
	int i = 0;

	for (i = 0;i < count;i ++) {
		dns_async_client_commit(ctx, domain[i], dns_async_client_result_callback);
		//sleep(2);
	}

	getchar();
#endif
	
}







