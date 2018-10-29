#include <stdio.h>
#include <stdbool.h>
#include <string.h>
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

typedef struct count_node {
    int count;
    char uri[MAXLINE];
    pthread_rwlock_t rwlock;
    struct count_node* next;
}count_node;

typedef struct cache_object {
    char uri[MAXLINE];
    char* data;

    //int timestamp; //necessary?
    pthread_rwlock_t rwlock;
    struct cache_object *next;

}cache_object;

int LFU_cache_count, LFU_cache_size, LRU_cache_size = 0;
// int timer = 0;
count_node* clist_head;
cache_object *LRU_cache_start; /* Head pointer for cache list */
cache_object *LFU_cache_start; /* Head pointer for cache list */

//pthread_rwlock_t rwlock; /* Lock to synchronise the adding of new nodes */

char current_uri1[MAXLINE], current_uri2[MAXLINE], current_uri3[MAXLINE];
char previous_uri1[MAXLINE], previous_uri2[MAXLINE], previous_uri3[MAXLINE];

void doit(int connfd);
void parse_uri(char *uri, struct uri_content *content, bool* is_dynamic);
bool read_requesthdrs(rio_t *rp, char *header_buf);

void write_to_cache(char *uri, char *data, int size);
cache_object* LFU_cache_update_needed();
cache_object *check_cache_hit(char *uri);
void read_cache_data(cache_object *cp, char *response);
void cache_insert_at_end(cache_object *cp, char *uri, char *data, int size);

void count_insert_at_end(count_node *head, char *uri);
count_node* find_count_node(count_node* head, char* uri);
void update_current_top_three(count_node* head);
void thread_wrapper(void *vargs);
void sig_handler(int sig);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);


int main(int argc, char **argv){

    Signal(SIGPIPE, sig_handler); // Ignore SIGPIPE

    int listenfd;
    int* connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    clientlen = sizeof(clientaddr);
    pthread_t tid;

    /* Initialize locks to maintain thread-safe operation */
    //pthread_rwlock_init(&(rwlock), NULL);

    /* Initialize head of count list */
    clist_head = (count_node*) malloc(sizeof(count_node));
    pthread_rwlock_init(&(clist_head->rwlock), NULL);

    LRU_cache_start = (cache_object*) malloc(sizeof(cache_object));
    LFU_cache_start = (cache_object*) malloc(sizeof(cache_object));

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

    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE], hdr_data[MAXLINE], new_request[MAXBUF];
    char response[380000]; /* Large enough to handle beeg 354kb without causing EXC_BAD_ACCESS */

    rio_t rio;
    struct uri_content content;
    int clientfd;
    bool is_dynamic, host_hdr_recvd;

    /* Increment timer with every request for cache purposes */
    //timer++;

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

    /* Update counts to help maintain LFU cache. Ignore dynamic content since that should not be cached */
    if(!is_dynamic) {
        count_node *search_result;
        search_result = find_count_node(clist_head, uri);
        if (search_result == NULL) {
            count_insert_at_end(clist_head, uri);
        }
    }

    count_node* debug = clist_head; // used for debugging global var head

    /* Check if URI is in LFU or LRU cache. If so, serve cache from response */
    cache_object* search = check_cache_hit(uri);
    if (!is_dynamic && search != NULL){

        pthread_rwlock_wrlock(&search->rwlock);

        int response_len = strlen(search->data);
        strncpy(response, search->data, response_len);

        Rio_writen(connfd, response, response_len);

        pthread_rwlock_unlock(&search->rwlock);
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
        int response_size = Rio_readn(clientfd, response, sizeof(response));

        /* Extract content length from response to determine if it should be cached*/
        char* c_index = strstr(response, "Content-");
        char content_len[MAXLINE];
        sscanf(c_index,"%*[^:]:%s", content_len);

        if(!is_dynamic && atoi(content_len) < MAX_OBJECT_SIZE)
            write_to_cache(uri, response, response_size);

        //Forward the response to the client
        Rio_writen(connfd, response, response_size);
        Close(clientfd);
    }
}

/*  parse_uri - read HTTP request headers */
void parse_uri(char *uri, struct uri_content *content, bool* is_dynamic){

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
bool read_requesthdrs(rio_t *rp, char *header_buf){

    char buf[MAXLINE];
    bool host_header_present = false;

    do {
        Rio_readlineb(rp, buf, MAXLINE);

        if(strcmp(buf, "\r\n") == 0)
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

    }while(strcmp(buf, "\r\n") != 0);
    return host_header_present;

}

void write_to_cache(char *uri, char *data, int size){

    if(size > MAX_OBJECT_SIZE)
        return;

    //pthread_rwlock_wrlock(&rwlock);
    update_current_top_three(clist_head);

    char* debugcurrent1 = &current_uri1;
    char* debugcurrent2 = &current_uri2;
    char* debugcurrent3 = &current_uri3;
    char* debugprev1 = &previous_uri1;
    char* debugprev2 = &previous_uri2;
    char* debugprev3 = &previous_uri3;
    cache_object* LFU_debug = LFU_cache_start;
    int LFU_cache_size_debug = LFU_cache_count;
    cache_object* LRU_debug = LRU_cache_start;
    int LRU_cache_size_debug = LRU_cache_size;

    /* If LFU cache size < 3, insert object into LFU cache */
    if(LFU_cache_count < 99) {
        cache_insert_at_end(LFU_cache_start, uri, data, size);
    }
    else{
        /* If top 3 have changed, overwrite the "victim" that fell off with the object */
        cache_object* victim = LFU_cache_update_needed();
        if(victim != NULL){
            pthread_rwlock_wrlock(&victim->rwlock);
            strcpy(victim->uri,uri);
            LFU_cache_size -= strlen(victim->data);
            free(victim->data);
            victim->data = malloc(size);
            strcpy(victim->data, data);
            LFU_cache_size += size;
            pthread_rwlock_unlock(&victim->rwlock);
        }

        // instead of count, we should be checking cache size? MAX_CACHE_SIZE

        /* If LRU cache size < 997, insert object into LRU cache */
        else if(LRU_cache_size < 997) {
            cache_insert_at_end(LRU_cache_start, uri, data, size);
            LRU_cache_size++;
            return;
        }

        //Determine if LRU cache needs eviction

        //Cache is not full - Add a new block without eviction
        else if( size <= MAX_CACHE_SIZE) {
            ;
        }
    }


    strcpy(previous_uri1, current_uri1);
    strcpy(previous_uri2, current_uri2);
    strcpy(previous_uri3, current_uri3);

    //pthread_rwlock_unlock(&rwlock);
}

void cache_insert_at_end(cache_object *cp, char *uri, char *data, int size){

    //pthread_rwlock_wrlock(&rwlock);

    cache_object* iterator = cp;
    while(iterator->next != NULL){
        iterator = iterator->next;
    }

    cache_object* newNode = (cache_object*) malloc(sizeof(cache_object));
    iterator->next = newNode;
    strcpy(newNode->uri, uri);
    newNode->data = malloc(size);

    strcpy(newNode->data, data);
    pthread_rwlock_init(&(newNode->rwlock), NULL);

    // Adjust global cache size variables
    if(cp == LFU_cache_start){
        LFU_cache_size += size;
        LFU_cache_count++;
    }
    else if(cp == LRU_cache_start){
        LRU_cache_size += size;
    }

    //pthread_rwlock_unlock(&rwlock);
}


cache_object* LFU_cache_update_needed(){

    char* debugcurrent1 = &current_uri1;
    char* debugcurrent2 = &current_uri2;
    char* debugcurrent3 = &current_uri3;
    char* debugprev1 = &previous_uri1;
    char* debugprev2 = &previous_uri2;
    char* debugprev3 = &previous_uri3;

    char* victim = NULL;

    if( strcmp(&previous_uri1,&current_uri1)!=0 && strcmp(&previous_uri1,&current_uri2)!=0
                                                 && strcmp(&previous_uri1,&current_uri3!=0)){
        victim = previous_uri1;
    }

    else if( strcmp(&previous_uri2,&current_uri1)!=0 && strcmp(&previous_uri2,&current_uri2)!=0 && strcmp(&previous_uri2,&current_uri3!=0)){
        victim = previous_uri2;
    }

    else if( strcmp(&previous_uri3,&current_uri1)!=0 && strcmp(&previous_uri3,&current_uri2)!=0 && strcmp(&previous_uri3,&current_uri3!=0)){
        victim = previous_uri3;
    }

    if(victim == NULL)
        return NULL;
    else
        return check_cache_hit(victim);
}






cache_object* check_cache_hit(char *uri){

    cache_object* iterator = LFU_cache_start;
    while(iterator != NULL){
        pthread_rwlock_wrlock(&iterator->rwlock);

        if(strcmp(iterator->uri, uri) == 0){
            pthread_rwlock_unlock(&iterator->rwlock);
            return iterator;
        }
        pthread_rwlock_unlock(&iterator->rwlock);
        iterator = iterator->next;
    }

    iterator = LRU_cache_start;
    while(iterator != NULL){
        pthread_rwlock_wrlock(&iterator->rwlock);

        if(strcmp(iterator->uri, uri) == 0){
            pthread_rwlock_unlock(&iterator->rwlock);
            return iterator;
        }
        pthread_rwlock_unlock(&iterator->rwlock);
        iterator = iterator->next;
    }

    return NULL;
}



void count_insert_at_end(count_node *head, char *uri){

    //pthread_rwlock_wrlock(&rwlock);

    count_node* iterator = head;
    while(iterator->next != NULL){
        iterator = iterator->next;
    }
    count_node* insert = (count_node*) malloc(sizeof(count_node));
    insert->count = 1;
    strcpy(insert->uri, uri);
    iterator->next = insert;
    pthread_rwlock_init(&(insert->rwlock), NULL);

    //pthread_rwlock_unlock(&rwlock);
}

count_node* find_count_node(count_node* head, char* uri){

    //pthread_rwlock_wrlock(&rwlock);

    count_node* iterator = head;
    while(iterator != NULL){
        if(!strcmp(iterator->uri,uri)) {
            pthread_rwlock_wrlock(&iterator->rwlock);
            iterator->count++;  /* If found, increment count */
            pthread_rwlock_unlock(&iterator->rwlock);
            return iterator;
        }
        else
            iterator = iterator->next;
    }
    //pthread_rwlock_unlock(&rwlock);
    return NULL;
}

void update_current_top_three(count_node* head){

    int count1, count2, count3 = 0;
    char uri1[MAXLINE], uri2[MAXLINE], uri3[MAXLINE];

    count_node* iterator = head;
    while(iterator != NULL){
        pthread_rwlock_wrlock(&iterator->rwlock);
        if(iterator->count > count1){
            count1 = iterator->count;
            strcpy(uri1, iterator->uri);
        }
        pthread_rwlock_unlock(&iterator->rwlock);
        iterator = iterator->next;
    }

    iterator = head;
    while(iterator != NULL){
        pthread_rwlock_wrlock(&iterator->rwlock);
        if(iterator->count > count2 && strcmp(iterator->uri,uri1)){
            count2 = iterator->count;
            strcpy(uri2 ,iterator->uri);
        }
        pthread_rwlock_unlock(&iterator->rwlock);
        iterator = iterator->next;
    }

    iterator = head;
    while(iterator != NULL){
        pthread_rwlock_wrlock(&iterator->rwlock);
        if(iterator->count > count3 && strcmp(iterator->uri,uri1) && strcmp(iterator->uri,uri2)){
            count3 = iterator->count;
            strcpy(uri3, iterator->uri);
        }
        pthread_rwlock_unlock(&iterator->rwlock);
        iterator = iterator->next;
    }

    strcpy(current_uri1, uri1);
    strcpy(current_uri2, uri2);
    strcpy(current_uri3, uri3);

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
    printf("SIGPIPE signal trapped. Exiting thread.\n");
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
