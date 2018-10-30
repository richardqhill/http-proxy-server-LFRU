#ifndef HW2_HANDOUT_PROXY_H
#define HW2_HANDOUT_PROXY_H

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
    struct count_node* next;
}count_node;

typedef struct cache_object {
    char uri[MAXLINE];
    char* data;
    int data_len;
    int content_len;
    struct cache_object *next;
}cache_object;

void doit(int connfd);
bool parse_uri(char *uri, struct uri_content *content);
bool read_requesthdrs(rio_t *rp, char *header_buf);

cache_object *search_caches(char *uri);
void write_to_cache(char *uri, char *data, int response_size, int content_len);
void cache_insert_at_end(cache_object *cp, char *uri, char *data, int response_size, int content_len);
cache_object* LFU_cache_update_needed();
void evict_oldest_from_LRU();

void count_insert_at_end(count_node *head, char *uri);
count_node* find_count_node(count_node* head, char* uri);
void update_current_top_three(count_node* head);

void thread_wrapper(void *vargs);
void sig_handler(int sig);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);

#endif //HW2_HANDOUT_PROXY_H
