#include <stdio.h>
#include <stdbool.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";


/* Structure for populating the content from the uri */
struct req_content {
    char host[MAXLINE];
    char path[MAXLINE];
    int port;
};



void doit(int fd);
void parse_uri(char *uri, struct req_content *content);
bool read_requesthdrs(rio_t *rp, char *data);
void clienterror(int fd, char *cause, char *errnum,
                 char *shortmsg, char *longmsg);


int main(int argc, char **argv)
{
    int listenfd, connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;

    /* Check command line args */
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    printf("received port# %s\n",argv[1]);

    listenfd = Open_listenfd(argv[1]);
    while (1) {
        clientlen = sizeof(clientaddr);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *) &clientaddr, clientlen, hostname, MAXLINE,
                    port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        doit(connfd);
        Close(connfd);
    }
}


void doit(int fd) {

    struct stat sbuf;

    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char hdr_data[MAXLINE], new_request[MAXBUF], response[1<<15]; //bitwise shift?

    rio_t rio;
    struct req_content content;

    int clientfd;
    bool is_dynamic, host_mentioned;


    //timer++;

    /* Read request line and headers */
    Rio_readinitb(&rio, fd);
    if (!Rio_readlineb(&rio, buf, MAXLINE))
        return;
    printf("%s\n", buf);
    sscanf(buf, "%s %s %s", method, uri, version);

    /* Proxy returns error if HTTP Request is NOT GET */
    if (strcasecmp(method, "GET")) {
        clienterror(fd, method, "501", "Not Implemented",
                    "Proxy does not implement this method");
        return;
    }

    /* if requested resource is in the "cgi-bin" directory, then content is dynamic */
    if(strstr(uri, "cgi-bin"))
        is_dynamic = true;

    /* Parse HTTP request: extract hostname, path and port into content struct */
    parse_uri(uri, &content);


    /* check if in cache to do */
    if (0){                                      //IMPLEMENT THIS!! :)
        ;
    }
    else{ /* not in cache, get resource from host server */



        //Parse the request headers.
        host_mentioned = read_requesthdrs(&rio, hdr_data);







        //Create new connection with server
        clientfd = Open_clientfd(content.host, content.port);

        //Write HTTP request to the server
        Rio_writen(clientfd, new_request, sizeof(new_request));

    }



}





/*  parse_uri - read HTTP request headers */
void parse_uri(char *uri, struct req_content *content)
{
    char temp[MAXLINE];

    //Extract path to resource
    if(strstr(uri,"http://") != NULL) {
        sscanf(uri, "http://%[^/]%s", temp, content->path);
        printf("Received path %s\n", content->path);
    }
    else
        sscanf(uri, "%[^/]%s", temp, content->path);

    //Extract port number and hostname
    if(strstr(temp, ":") != NULL) {
        sscanf(temp, "%[^:]:%d", content->host, &content->port);
        printf("Received port %s\n", content->port);
    }
    else {
        strcpy(content->host,temp);
        content->port = 80;
    }

    // in case the path to resource is empty
    if(!content->path[0])
        strcpy(content->path,"./");

}


/*  read_requesthdrs - read HTTP request headers */
bool read_requesthdrs(rio_t *rp, char *data)
{
    char buf[MAXLINE];

    Rio_readlineb(rp, buf, MAXLINE);
    printf("%s", buf);
    while(strcmp(buf, "\r\n")) {
        Rio_readlineb(rp, buf, MAXLINE);
        printf("%s", buf);
    }
    return true;
}












/* clienterror - returns an error message to the client */
void clienterror(int fd, char *cause, char *errnum,
                 char *shortmsg, char *longmsg)
{
    char buf[MAXLINE], body[MAXBUF];

    /* Build the HTTP response body */
    sprintf(body, "<html><title>Tiny Error</title>");
    sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

    /* Print the HTTP response */
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));
}
