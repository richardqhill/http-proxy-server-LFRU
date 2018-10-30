#include "proxy.h"

int LFU_cache_count, LFU_cache_size, LRU_cache_size = 0;

count_node* clist_head;
cache_object *LRU_cache_start; /* Head pointer for cache list */
cache_object *LFU_cache_start; /* Head pointer for cache list */

pthread_rwlock_t rwlock; /* Lock to synchronise the access of global vars like clisthead and cache starts */

char current_uri1[MAXLINE], current_uri2[MAXLINE], current_uri3[MAXLINE];
char previous_uri1[MAXLINE], previous_uri2[MAXLINE], previous_uri3[MAXLINE];


int main(int argc, char **argv){

    Signal(SIGPIPE, sig_handler); // Ignore SIGPIPE

    int listenfd;
    int* connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    clientlen = sizeof(clientaddr);
    pthread_t tid;

    /* Initialize head of count list */
    clist_head = (count_node*) malloc(sizeof(count_node));
    LRU_cache_start = (cache_object*) malloc(sizeof(cache_object));
    LFU_cache_start = (cache_object*) malloc(sizeof(cache_object));

    /* Initialize locks to maintain thread-safe operation */
    pthread_rwlock_init(&rwlock, NULL);
    pthread_rwlock_init(&LRU_cache_start->rwlock, NULL);
    pthread_rwlock_init(&LFU_cache_start->rwlock, NULL);

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

/* Main routine to be called by threads */
void doit(int connfd) {
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE], hdr_data[MAXLINE], new_request[MAXBUF];
    char response[380000]; /* Large enough to handle beeg 354kb without causing EXC_BAD_ACCESS */
    struct uri_content content;
    int clientfd;
    bool is_dynamic, host_hdr_recvd;
    rio_t rio;

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
    is_dynamic = parse_uri(uri, &content);

    /* Update counts to help maintain LFU cache. Ignore dynamic content since that should not be cached */
    if(!is_dynamic) {
        count_node *search_result;
        search_result = find_count_node(clist_head, uri);
        if (search_result == NULL) {
            count_insert_at_end(clist_head, uri);
        }
    }

    /* Check if URI is in LFU or LRU cache. If so, serve cache from response */
    cache_object* search = search_caches(uri);
    if (!is_dynamic && search != NULL){

        int response_len = search->data_len;
        memcpy(response, search->data, response_len);
        Rio_writen(connfd, response, response_len);

        /* If this read from cache changed which 3 objects should be in LFU, update LFU */
        //pthread_rwlock_wrlock(&rwlock);

        update_current_top_three(clist_head);
        cache_object* victim = LFU_cache_update_needed();
        if(victim != NULL){

            pthread_rwlock_wrlock(&victim->rwlock);

            strcpy(victim->uri,uri);
            LFU_cache_size -= victim->content_len;
            free(victim->data);
            victim->data = malloc(response_len);
            memcpy(victim->data, response, response_len);
            LFU_cache_size += search->content_len;

            pthread_rwlock_unlock(&victim->rwlock);
        }

        //pthread_rwlock_unlock(&rwlock);
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
        clientfd = Open_clientfd((char *)&content.host, (char *)&content.port);
        Rio_writen(clientfd, new_request, sizeof(new_request));

        /* Read response from server */
        int response_size = Rio_readn(clientfd, response, sizeof(response));

        /* Extract content length from response */
        char* c_index = strstr(response, "Content-");
        char content_len[MAXLINE];
        sscanf(c_index,"%*[^:]:%s", content_len);

        /* Write response to cache if content is not dynamic and response size < MAX_OBJECT_SIZE */
        if(!is_dynamic && response_size <= MAX_OBJECT_SIZE) {
            write_to_cache(uri, response, response_size, atoi(content_len));
        }

        /* Forward the response to the client */
        Rio_writen(connfd, response, response_size);
        Close(clientfd);
    }
}

/* Parse URI into uri_content struct and return if URI is for dynamic content */
bool parse_uri(char *uri, struct uri_content *content){

    char temp[MAXLINE];

    /* Extract the path to the resource */
    if(strstr(uri,"http://") != NULL)
        sscanf( uri, "http://%[^/]%s", temp, content->path);
    else
        sscanf( uri, "%[^/]%s", temp, content->path);

    /* Extract the port number and the hostname */
    if( strstr(temp, ":") != NULL)
        sscanf(temp,"%[^:]:%s", content->host, content->port);
    else {
        strcpy(content->host,temp);
        strcpy(content->port, "80");
    }

    /* If the path to resource is empty, add ./ */
    if(!content->path[0])
        strcpy(content->path,"./");

    /* If requested resource is in the "cgi-bin" directory, then content is dynamic */
    if(strstr(uri, "cgi-bin"))
        return true;
    else
        return false;
}

/* Read HTTP headers into header_buf and return if host header was present */
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

/* See if given URI is in LFU or LRU cache. Return pointer to cache object if present */
cache_object* search_caches(char *uri){

    //pthread_rwlock_wrlock(&rwlock);

    cache_object* iterator = LFU_cache_start;
    while(iterator != NULL){
        if(strcmp(iterator->uri, uri) == 0){
            //pthread_rwlock_unlock(&rwlock);
            return iterator;
        }
        cache_object* temp = iterator->next;
        iterator = temp;
    }

    iterator = LRU_cache_start;
    while(iterator != NULL){

        if(strcmp(iterator->uri, uri) == 0){
            //pthread_rwlock_unlock(&rwlock);
            return iterator;
        }
        cache_object* temp = iterator->next;
        iterator = temp;
    }

    //pthread_rwlock_unlock(&rwlock);
    return NULL;
}

/* Write the server response into LFU or LRU cache */
void write_to_cache(char *uri, char *data, int response_size, int content_len) {

    if(response_size > MAX_OBJECT_SIZE)
        return;

    //pthread_rwlock_wrlock(&rwlock);

    update_current_top_three(clist_head);

    /* If LFU cache size < 3, insert object into LFU cache */
    if(LFU_cache_count < 3) {
        cache_insert_at_end(LFU_cache_start, uri, data, response_size, content_len);
    }
    else{
        /* If top 3 have changed, overwrite the "victim" that fell off with the new object */
        cache_object* victim = LFU_cache_update_needed();
        if(victim != NULL){

            pthread_rwlock_wrlock(&victim->rwlock);

            strcpy(victim->uri,uri);
            LFU_cache_size -= victim->content_len;
            free(victim->data);
            victim->data = malloc(response_size);
            memcpy(victim->data, data, response_size);
            LFU_cache_size += content_len;

            pthread_rwlock_unlock(&victim->rwlock);
        }
        else{ /* Top 3 have not changed, write object to LRU */
            while(LRU_cache_size + LFU_cache_size + response_size > MAX_CACHE_SIZE){
                evict_oldest_from_LRU();
            }
            cache_insert_at_end(LRU_cache_start, uri, data, response_size, content_len);
        }
    }
    strcpy(previous_uri1, current_uri1);
    strcpy(previous_uri2, current_uri2);
    strcpy(previous_uri3, current_uri3);
    //pthread_rwlock_unlock(&rwlock);
}

/* Insert a new cache object at the end of the LFU or LRU cache. */
void cache_insert_at_end(cache_object *cp, char *uri, char *data, int response_size, int content_len) {

    cache_object* iterator = cp;
    while(iterator->next != NULL){
        cache_object* temp = iterator->next;
        iterator = temp;
    }

    cache_object* newNode = (cache_object*) malloc(sizeof(cache_object));
    strcpy(newNode->uri, uri);
    newNode->data = malloc(response_size);
    newNode->data_len = response_size;
    newNode->content_len = content_len;
    memcpy(newNode->data, data, response_size);
    newNode->next = NULL;
    pthread_rwlock_init(&(newNode->rwlock), NULL);

    pthread_rwlock_wrlock(&iterator->rwlock);
    iterator->next = newNode;
    pthread_rwlock_unlock(&iterator->rwlock);

    // Adjust global cache size variables
    if(cp == LFU_cache_start){
        LFU_cache_size += content_len;
        LFU_cache_count++;
    }
    else if(cp == LRU_cache_start){
        LRU_cache_size += content_len;
    }
}

/* If top 3 most frequent objects has changed, return pointer to object that fell out of top 3
 * This function is only called when LFU size >=3, so do not need to worry about nulls */
cache_object* LFU_cache_update_needed(){

    char* victim = NULL;

    if( strcmp(previous_uri1,current_uri1)!=0 && strcmp(previous_uri1,current_uri2)!=0
        && strcmp(previous_uri1,current_uri3)!=0)
        victim = previous_uri1;
    else if( strcmp(previous_uri2,current_uri1)!=0 && strcmp(previous_uri2,current_uri2)!=0
             && strcmp(previous_uri2,current_uri3)!=0)
        victim = previous_uri2;
    else if( strcmp(previous_uri3,current_uri1)!=0 && strcmp(previous_uri3,current_uri2)!=0
             && strcmp(previous_uri3,current_uri3)!=0)
        victim = previous_uri3;

    if(victim == NULL) {

        return NULL;
    }
    else {

        return search_caches(victim);
    }
}

/* Remove oldest node from LRU cache (first node after LRU head) */
void evict_oldest_from_LRU(){

    //pthread_rwlock_wrlock(&rwlock);

    cache_object* victim = LRU_cache_start->next;

    pthread_rwlock_wrlock(&victim->rwlock);

    LRU_cache_size -= victim->content_len;
    free(victim->data);
    LRU_cache_start->next = LRU_cache_start->next->next;

    pthread_rwlock_unlock(&victim->rwlock);

    //pthread_rwlock_unlock(&rwlock);
}

/* Return pointer to count node for given URI if we have one */
count_node* find_count_node(count_node* head, char* uri){

    //pthread_rwlock_wrlock(&rwlock);

    count_node* iterator = head;
    while(iterator != NULL){
        if(!strcmp(iterator->uri,uri)) {
            iterator->count++;  /* If found, increment count */

            //pthread_rwlock_unlock(&rwlock);
            return iterator;
        }
        else {
            iterator = iterator->next;
        }
    }
    //pthread_rwlock_unlock(&rwlock);
    return NULL;
}

/* Add a new count node to the end of our count list */
void count_insert_at_end(count_node *head, char *uri){

    pthread_rwlock_wrlock(&rwlock);

    count_node* iterator = head;
    while(iterator->next != NULL){
        iterator = iterator->next;
    }

    count_node* insert = (count_node*) malloc(sizeof(count_node));
    insert->count = 1;
    strcpy(insert->uri, uri);
    insert->next = NULL;

    iterator->next = insert;

    pthread_rwlock_unlock(&rwlock);
}

/* Iterate through count list and determine the top 3 most frequent URI's. Update globals */
void update_current_top_three(count_node* head){

    int count1, count2, count3 = 0;
    char uri1[MAXLINE];
    char uri2[MAXLINE];
    char uri3[MAXLINE];
    memset(&uri1,0,MAXLINE);
    memset(&uri2,0,MAXLINE);
    memset(&uri3,0,MAXLINE);

    count_node* iterator = head;
    while(iterator != NULL){
        if(iterator->count > count1){
            count1 = iterator->count;
            strcpy(uri1, iterator->uri);
        }
        iterator = iterator->next;
    }

    iterator = head;
    while(iterator != NULL){
        if(iterator->count > count2 && strcmp(iterator->uri,uri1)!= 0){
            count2 = iterator->count;
            strcpy(uri2 ,iterator->uri);
        }
        iterator = iterator->next;
    }

    iterator = head;
    while(iterator != NULL){
        if(iterator->count > count3 && strcmp(iterator->uri,uri1)!=0 && strcmp(iterator->uri,uri2)!=0){
            count3 = iterator->count;
            strcpy(uri3, iterator->uri);
        }
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

/* Signal handler to gracefully handle thread failure */
void sig_handler(int sig) {
    printf("SIGPIPE signal trapped. Exiting thread.\n");
    Pthread_exit(NULL);
}

/* Returns an error message to the client */
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
