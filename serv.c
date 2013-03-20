#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h> //stat
#include <sys/mman.h>
#include <netinet/in.h> //sockaddr_in
#include "rio.h"

#define LISTENQ 1024
#define MAXLINE 4096
#define NUM_THREAD 16
#define ARRAY_SIZE(array) (sizeof(array) / sizeof(array[0]))

extern char **environ;

union usa {
	struct sockaddr sa;
	struct sockaddr_in sin;
};

struct socket {
	int sock;
	union usa lsa;
	union usa rsa;
};

struct tws_context {
	int port;

	volatile int num_threads;
	pthread_mutex_t mutex;
	pthread_cond_t 	cond;

	struct socket queue[20];
	volatile int sq_head;
	volatile int sq_tail;
	pthread_cond_t sq_full;
	pthread_cond_t sq_empty;

	int stop_flag;
};

int open_listenfd(int port);
void doit(int fd);
void read_requesthdrs(rio_t* fd);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
void sighandler(int signo);

typedef void *(tws_thread_func_t)(void *);
static int tws_start_thread(tws_thread_func_t func, void *param) {
	pthread_t thread_id;
	pthread_attr_t attr;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	return pthread_create(&thread_id, &attr, func, param);
}

int open_listenfd(int port){
	int 				listenfd,optval=1;
	struct sockaddr_in	serveraddr;

	if((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0){
		return -1;
	}

	if(setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
					(const void*)&optval,sizeof(optval)) < 0)
		return -1;

	memset(&serveraddr,0,sizeof(serveraddr));
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
	serveraddr.sin_port = htons((unsigned short)port);
	if(bind(listenfd,(struct sockaddr*)&serveraddr, sizeof(serveraddr)) < 0)
		return -1;

	if(listen(listenfd, LISTENQ) < 0)
		return -1;
	return listenfd;
}

void doit(int fd)
{
	int is_static;
	struct stat sbuf;
	char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
	char filename[MAXLINE], cgiargs[MAXLINE];
	rio_t rio;

	rio_readinitb(&rio, fd);
	rio_readlineb(&rio, buf, MAXLINE);
	sscanf(buf,"%s %s %s",method, uri, version);
	printf("%s",buf);
	if(strcasecmp(method, "GET")) {
		clienterror(fd, method, "501", "Not Implemented",
					"Taiweisuo dose not implement this method");
		return;
	}
	read_requesthdrs(&rio);

	is_static = parse_uri(uri,filename, cgiargs);
	if(stat(filename, &sbuf) < 0) {
		clienterror(fd, filename, "404", "Not found",
					"Taiweisuo couldn't find the file");
		return;
	}

	if(is_static) {
		if(!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {
			clienterror(fd, filename, "403", "Forbidden",
					"Taiweisuo couldn't read the file");
			return;
		}
		serve_static(fd, filename, sbuf.st_size);
	}
	else {
		if( !(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)) {
			clienterror(fd, filename, "403", "Forbidden",
					"Taiweisuo couldn't read the file");
			return;
		}
		serve_dynamic(fd, filename, cgiargs);
	}
}

void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg)
{
	char buf[MAXLINE], body[MAXLINE];

	sprintf(body, "<html><head><title>Taiweisuo Error</title></head>");
	sprintf(body, "%s<body bgcolor=\"FFFFFF\">\r\n", body);
	sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
	sprintf(body, "%s<p>%s: %s</p>\r\n",body, longmsg, cause);
	sprintf(body, "%s<hr><em>The Taiweisuo Web Server</em>\r\n",body);

	sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
	rio_writen(fd, buf, strlen(buf));
	sprintf(buf, "Content-type: text/html\r\n");
	rio_writen(fd, buf, strlen(buf));
	sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
	rio_writen(fd, buf, strlen(buf));
	rio_writen(fd, body, strlen(body));
}

void read_requesthdrs(rio_t *rp)
{
	char buf[MAXLINE];

	ssize_t nread = rio_readlineb(rp, buf, MAXLINE);
	while(nread != 0 && strcmp(buf,"\r\n")) {
		printf("%s", buf);
		nread = rio_readlineb(rp, buf, MAXLINE);
	}
	putchar(10);
	return;
}

int parse_uri(char *uri, char *filename, char *cgiargs)
{
	char *ptr;

	if(!strstr(uri, "cgi-bin")) {
		strcpy(cgiargs,"");
		strcpy(filename, "./www");
		strcat(filename, uri);
		if(uri[strlen(uri) - 1] == '/')
			strcat(filename, "index.html");
		return 1;
	}
	else {
		ptr = index(uri, '?');
		if(ptr) {
			strcpy(cgiargs, ptr+1);
			*ptr = '\0';
		}
		else
			strcpy(cgiargs, "");
		strcpy(filename, "./www");
		strcat(filename, uri);
		return 0;
	}
}	

void serve_static(int fd, char *filename, int filesize)
{
	int srcfd;
	char *srcp, filetype[MAXLINE], buf[MAXLINE];

	get_filetype(filename, filetype);
	sprintf(buf, "HTTP/1.0 200 OK\r\n");
	sprintf(buf, "%sServer: Taiweisuo Web Server\r\n",buf);
	sprintf(buf, "%sContent-length: %d\r\n", buf, filesize);
	sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype);
	rio_writen(fd, buf, strlen(buf));

	srcfd = open(filename, O_RDONLY, 0);
	srcp = mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
	close(srcfd);
	rio_writen(fd, srcp, filesize);
	munmap(srcp, filesize);
}

void get_filetype(char *filename, char *filetype)
{
	if(strstr(filename, ".html"))
		strcpy(filetype, "text/html");
	else if (strstr(filename, ".gif"))
		strcpy(filetype, "image/gif");
	else if (strstr(filename, ".jpg"))
		strcpy(filetype, "image/jpg");
	else if (strstr(filename, ".png"))
		strcpy(filetype, "image/png");
	else if (strstr(filename, ".js"))
		strcpy(filetype, "text/javascript");
	else if (strstr(filename, ".css"))
		strcpy(filetype, "text/css");
	else
		strcpy(filetype, "text/plain");
}

void serve_dynamic(int fd, char *filename, char *cgiargs)
{
	char buf[MAXLINE], *emptylist[] = {NULL};

	sprintf(buf, "HTTP/1.0 200 OK\r\n");
	rio_writen(fd, buf, strlen(buf));
	sprintf(buf, "Server: Taiweisuo Web Server\r\n");
	rio_writen(fd, buf, strlen(buf));

	if(fork() == 0) {
		setenv("QUERY_STRING", cgiargs , 1);
		dup2(fd, STDOUT_FILENO);
		execve(filename, emptylist, environ);
	}
	wait(NULL);
}

static void *consume_socket(struct tws_context *ctx, struct socket *sp) {
	pthread_mutex_lock(&ctx->mutex);
	int sz = ARRAY_SIZE(ctx->queue);

	while(ctx->stop_flag == 0 && ctx->sq_head == ctx->sq_tail) {
		pthread_cond_wait(&ctx->sq_full, &ctx->mutex);
	}

	if(ctx->sq_tail > ctx->sq_head) {
		*sp = ctx->queue[ctx->sq_head % sz];
		ctx->sq_head++;
		if(ctx->sq_head >= sz) {
			ctx->sq_head -=sz;
			ctx->sq_tail -=sz;
		}
	}
	pthread_cond_signal(&ctx->sq_empty);
	pthread_mutex_unlock(&ctx->mutex);
}	

static void *worker_thread(void *arg) {
	struct tws_context *ctx = (struct tws_context*) arg;
	struct socket client;

	while(1) {
		consume_socket(ctx, &client);
		printf("Thread %u handling\n",(unsigned int)pthread_self());
		doit(client.sock);
		close(client.sock);
	}
	return NULL;
}

static void produce_socket(struct tws_context *ctx, struct socket *sp) {
	pthread_mutex_lock(&ctx->mutex);
	int sz = ARRAY_SIZE(ctx->queue);
	
	while(ctx->stop_flag == 0 && ctx->sq_tail - ctx->sq_head >= sz) {
		pthread_cond_wait(&ctx->sq_empty, &ctx->mutex);
	}

	if(ctx->sq_tail - ctx->sq_head < sz) {
		ctx->queue[ctx->sq_tail % sz] = *sp;
		ctx->sq_tail++;
	}
	pthread_cond_signal(&ctx->sq_full);
	pthread_mutex_unlock(&ctx->mutex);
}

static void *master_thread(void *arg) {
	struct tws_context *ctx = (struct tws_context *)arg;
	struct socket listenfd, so;
   	socklen_t len;
	struct sockaddr_in clientaddr;

	listenfd.sock = open_listenfd(ctx->port);
	while(1) {
		len = sizeof(clientaddr);
		if((so.sock = accept(listenfd.sock, (struct sockaddr*)&so.rsa.sa, &len)) < 0) {
		}
		else {
			getsockname(so.sock, &so.lsa.sa, &len);	
			produce_socket(ctx, &so);
		}
	}
	return NULL;
}

int main(int argc, char **argv)
{
	if(argc != 2) {
		fprintf(stderr,"usage: %s <port>\n", argv[0]);
		exit(1);
	}

	struct tws_context *ctx = malloc(sizeof(struct tws_context));

	ctx->port = atoi(argv[1]);
	ctx->sq_head = 0;
	ctx->sq_tail = 0;
	pthread_mutex_init(&ctx->mutex, NULL);
	pthread_cond_init(&ctx->sq_full, NULL);
	pthread_cond_init(&ctx->sq_empty, NULL);

	signal(SIGPIPE, SIG_IGN);

	int i;
	tws_start_thread(master_thread, ctx);
	for(i = 0;i < 20;i++) {
		tws_start_thread(worker_thread, ctx);
	}

	while( ctx->stop_flag != 1) {
		sleep(1);
	}
	free(ctx);
	return 0;
}
