#include <stdio.h>
#include <stdbool.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *connection_hdr = "Connection: close\r\n";
static const char *proxy_conn_hdr = "Proxy-Connection: close\r\n";

/* Structure for populating the content from the uri */
struct req_content {
    char host[MAXLINE];
    char path[MAXLINE];
    char port[MAXLINE];
};


void doit(int connfd);
void parse_uri(char *uri, struct req_content *content, bool* is_dynamic);

bool read_requesthdrs(rio_t *rp, char *header_buf);
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


void doit(int connfd) {

    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char hdr_data[MAXLINE], new_request[MAXBUF], response_hdrs_buf[1<<15]; /* bitwise shift */
    rio_t rio_req, rio_response;
    struct req_content content;
    int clientfd;
    bool is_dynamic, host_mentioned;

    //timer++;

    /* Read request line and headers */
    Rio_readinitb(&rio_req, connfd);
    if (!Rio_readlineb(&rio_req, buf, MAXLINE))
        return;
    printf("%s\n", buf);
    sscanf(buf, "%s %s %s", method, uri, version);

    /* Proxy returns error if HTTP Request is NOT GET */
    if (strcasecmp(method, "GET")) {
        clienterror(connfd, method, "501", "Not Implemented", "Proxy does not implement this method");
        return;
    }

    /* Parse URI: update dynamic flag and extract hostname, path and port into content struct */
    parse_uri(uri, &content, &is_dynamic);

    /* Check if URI is in cache */
    if (0){                                      //IMPLEMENT THIS!! :)
        char response[MAXBUF+MAX_OBJECT_SIZE];
    }
    else{ /* If not in cache, get resource from host server */
        /* Parse the request headers into hdr_data and return if host header was present */
        host_mentioned = read_requesthdrs(&rio_req, hdr_data);

        /* Begin generating new modified HTTP request to forward to the server */
        sprintf(new_request, "GET %s HTTP/1.0\r\n", content.path);

        /* If host was in header, use that. If not, grab host from URI */
        if(!host_mentioned)
            sprintf(new_request, "%sHost: %s\r\n", new_request, content.host);

        sprintf(new_request,"%s%s%s%s%s%s", new_request, hdr_data, user_agent_hdr,
                connection_hdr, proxy_conn_hdr, "\r\n");

        /* Create new connection with server and write HTTP request to server */
        clientfd = Open_clientfd(&content.host, &content.port);
        Rio_writen(clientfd, new_request, sizeof(new_request));




        /* Read response from server */
        /* We file all response headers into response_hdrs_buf, extract content length from the header and file the
         * rest of the response into response_body. We forward these to the requester */

        memset(buf, NULL, sizeof(buf));
        Rio_readinitb(&rio_response, clientfd);

        if (!Rio_readlineb(&rio_response, buf, MAXLINE))
            return;


        char content_length_line_buff[MAXLINE];
        char content_length_buff[1000];
        char temp[1000];

        /* Read all headers and extract content length */
        do{
            Rio_readlineb(&rio_response, buf, MAXLINE);
            sprintf(response_hdrs_buf,"%s%s",response_hdrs_buf,buf);

            if(strstr(buf, "Content-length:")){
                sprintf(content_length_line_buff, "%s", buf);
            }
        }while(strcmp(buf, "\r\n"));


        sscanf(content_length_line_buff,"%[^:]:%s", &temp, &content_length_buff);
        int content_length = atoi(content_length_buff);

        char resp_body_buf[1<<16]; // WHY DOESN'T CONTENT LENGTH WORK HERE
        int read_len = 0;
        do{
            read_len = Rio_readlineb(&rio_response, buf, MAXLINE);
            sprintf(resp_body_buf,"%s%s",resp_body_buf, buf);
        }while(read_len!=0);


        char complete_response[1<<16]; // WHY DOESN'T CONTENT LENGTH WORK HERE
        sprintf(complete_response,"%s%s",response_hdrs_buf,resp_body_buf);








        // CACHING TO DO

        //Forward the response to the client
        Rio_writen(connfd, complete_response, sizeof(complete_response));
        Close(clientfd);
    }

    //response_size = Rio_readn(clientfd, response, sizeof(response));

}





/*  parse_uri - read HTTP request headers */
void parse_uri(char *uri, struct req_content *content, bool* is_dynamic)
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
        strcpy(content->port, '80');
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
