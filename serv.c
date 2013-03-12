#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h> //sockaddr_in
#include <sys/stat.h> //stat
#include <sys/mman.h>
#include "rio.h"

#define LISTENQ 1024
#define MAXLINE 4096

extern char **environ;

int open_listenfd(int port);
void doit(int fd);
void read_requesthdrs(rio_t* fd);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);

int main(int argc, char **argv)
{
	int listenfd, connfd, port, clientlen;
	struct sockaddr_in clientaddr;

	if(argc != 2) {
		fprintf(stderr,"usage: %s <port>\n", argv[0]);
		exit(1);
	}

	port = atoi(argv[1]);

	listenfd = open_listenfd(port);
	while(1) {
		clientlen = sizeof(clientaddr);
		connfd = accept(listenfd, (struct sockaddr*)&clientaddr, &clientlen);
		doit(connfd);
		close(connfd);
	}
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

	rio_readlineb(rp, buf, MAXLINE);
	while(strcmp(buf,"\r\n")) {
		printf("%s", buf);
		rio_readlineb(rp, buf, MAXLINE);
	}
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
