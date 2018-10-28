#include <stdio.h>
#include <stdbool.h>
#include <limits.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

static const char *user_agent_hdr =
        "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *connection_hdr = "Connection: close\r\n";
static const char *proxy_conn_hdr = "Proxy-Connection: close\r\n";

/* Struct for populating the content from URI */
struct uri_content {
    char host[MAXLINE];
    char path[MAXLINE];
    char port[MAXLINE];
};

typedef struct cache_object {
    char uri[MAXLINE];
    char data[MAX_OBJECT_SIZE];
    int timestamp; //necessary?
    pthread_rwlock_t rwlock;
    struct cache_object *next;
}cache_object;

int cache_size = 0, timer = 0;
cache_object *cache_start; /* Head pointer for cache list */
pthread_rwlock_t cache_start_rwlock; /* Lock to synchronise the adding of new blocks to the cache */

void doit(int connfd);
void parse_uri(char *uri, struct uri_content *content, bool* is_dynamic);
bool read_requesthdrs(rio_t *rp, char *header_buf);
cache_object *check_cache_hit(char *uri);
void read_cache_data(cache_object *cp, char *response);
void write_to_cache(char *tag, char *data, int size);
void thread_wrapper(void *vargs);
void sig_handler(int sig);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);



int main(int argc, char **argv)
{
    Signal(SIGPIPE, sig_handler); // Ignore SIGPIPE

    int listenfd;
    int* connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    clientlen = sizeof(clientaddr);

    pthread_t tid;
    cache_start = NULL;

    //Initialize lock for cache_start to maintain thread-safe operation
    pthread_rwlock_init(&(cache_start_rwlock), NULL);

    /* Check command line args */
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    listenfd = Open_listenfd(argv[1]);
    while (1) {
        connfd = (int *) Malloc(sizeof(connfd));
        *connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *) &clientaddr, clientlen, hostname, MAXLINE,
                    port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);

        Pthread_create(&tid, NULL, (void *)thread_wrapper, connfd);
    }
    return 0;
}


void doit(int connfd) {

    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char hdr_data[MAXLINE], new_request[MAXBUF], response[1<<16]; /*Is there a max response size? */
    rio_t rio;
    struct uri_content content;
    int clientfd;
    bool is_dynamic, host_hdr_recvd;

    /* Increment timer with every request for cache purposes */
    timer++;

    /* Read request line and headers */
    Rio_readinitb(&rio, connfd);
    if (!Rio_readlineb(&rio, buf, MAXLINE))
        return;
    printf("%s\n", buf);
    sscanf(buf, "%s %s %s", method, uri, version);

    /* Proxy returns error if HTTP Request is not GET */
    if (strcasecmp(method, "GET")) {
        clienterror(connfd, method, "501", "Not Implemented", "Proxy does not implement this method");
        return;
    }

    /* Parse URI: update dynamic flag and extract hostname, path and port into content struct */
    parse_uri(uri, &content, &is_dynamic);

    /* Check if URI is in cache */
    if (!is_dynamic && 0){                                      //IMPLEMENT THIS!! :)


        ;


    }
    else{ /* If not in cache, get resource from host server */

        /* Parse the request headers into hdr_data and return if host header was present */
        host_hdr_recvd = read_requesthdrs(&rio, hdr_data);

        /* Begin generating new modified HTTP request to forward to the server */
        sprintf(new_request, "GET %s HTTP/1.0\r\n", content.path);

        /* If host was in header, use that. If not, grab host from URI */
        if(!host_hdr_recvd)
            sprintf(new_request, "%sHost: %s\r\n", new_request, content.host);

        sprintf(new_request,"%s%s%s%s%s%s", new_request, hdr_data, user_agent_hdr,
                connection_hdr, proxy_conn_hdr, "\r\n");

        /* Create new connection with server and write HTTP request to server */
        clientfd = Open_clientfd(&content.host, &content.port);
        Rio_writen(clientfd, new_request, sizeof(new_request));

        /* Read response from server */
        Rio_readn(clientfd, response, sizeof(response));

        /* Extract content length from response to determine if it should be cached*/
        char* c_index = strstr(response, "Content");
        char content_len[MAXLINE];
        sscanf(c_index,"%*[^:]:%s", content_len);


        // CACHING TO DO
        if(!is_dynamic && atoi(content_len) < MAX_OBJECT_SIZE)
            ;


        //Forward the response to the client
        Rio_writen(connfd, response, sizeof(response)); //throws warning because we don't actually write this much
        Close(clientfd);
    }
}



/*  parse_uri - read HTTP request headers */
void parse_uri(char *uri, struct uri_content *content, bool* is_dynamic)
{
    /* If requested resource is in the "cgi-bin" directory, then content is dynamic */
    if(strstr(uri, "cgi-bin"))
        *is_dynamic = true;

    char temp[MAXLINE];

    //Extract the path to the resource
    if(strstr(uri,"http://") != NULL)
        sscanf( uri, "http://%[^/]%s", temp, content->path);
    else
        sscanf( uri, "%[^/]%s", temp, content->path);

    //Extract the port number and the hostname
    if( strstr(temp, ":") != NULL)
        sscanf(temp,"%[^:]:%s", content->host, &content->port);
    else {
        strcpy(content->host,temp);
        strcpy(content->port, "80");
    }

    // in case the path to resource is empty
    if(!content->path[0])
        strcpy(content->path,"./");

}

/*  read_requesthdrs - read HTTP request headers, return whether or not host was present in headers */
bool read_requesthdrs(rio_t *rp, char *header_buf)
{
    char buf[MAXLINE];
    bool host_header_present = false;

    do {
        Rio_readlineb(rp, buf, MAXLINE);

        if(!strcmp(buf, "\r\n"))
            continue;

        /* Ignore these headers, we will substitute our own (defined at top of file) */
        if(strstr(buf, "User-Agent:") || strstr(buf, "Connection:") || strstr(buf, "Proxy-Connection:"))
            continue;

        if(strstr(buf, "Host:")) {
            sprintf(header_buf, "%s%s", header_buf, buf);
            host_header_present = true;
            continue;
        }

        /* Proxy will pass along any additional request headers unchanged */
        sprintf(header_buf, "%s%s", header_buf, buf);

    }while(strcmp(buf, "\r\n"));
    return host_header_present;

}


/* Thread-handler that calls the required functions to serve the requests. */
void thread_wrapper(void *vargs) {
    int fd = *(int *) vargs;
    Pthread_detach(pthread_self());
    Free(vargs);
    doit(fd);
    Close(fd);
    Pthread_exit(NULL);
}

void sig_handler(int sig) {
    printf("SIGPIPE signal trapped. Exiting thread.");
    Pthread_exit(NULL);
}

/* clienterror - returns an error message to the client */
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg){
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
