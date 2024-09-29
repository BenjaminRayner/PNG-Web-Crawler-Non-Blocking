#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <search.h>
#include <curl/multi.h>
#include <libxml/HTMLparser.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/uri.h>
#include <time.h>
#include <sys/time.h>

#define BUF_SIZE 1048576  /* 1024*1024 = 1M */
#define BUF_INC  524288   /* 1024*512  = 0.5M */

#define CT_PNG  "image/png"
#define CT_HTML "text/html"

#define max(a, b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

typedef struct {
    char *buf;       /* memory to hold a copy of received data */
    size_t size;     /* size of valid data in buf in bytes*/
    size_t max_size; /* max capacity of buf in bytes*/
    char* url;
} RECV_BUF;

htmlDocPtr mem_getdoc(char *buf, int size, const char *url);
xmlXPathObjectPtr getnodeset (xmlDocPtr doc, xmlChar *xpath);
int find_http(char *buf, int size, int follow_relative_links, const char *base_url);
size_t write_cb_curl3(char *p_recv, size_t size, size_t nmemb, void *p_userdata);
int recv_buf_init(RECV_BUF *ptr, size_t max_size);
int recv_buf_cleanup(RECV_BUF *ptr);
void cleanup(CURL *curl, RECV_BUF *ptr);
CURL *easy_handle_init(RECV_BUF *ptr);
void process_data(CURL *curl_handle, RECV_BUF *p_recv_buf);

bool logURLs = false;
char* frontier[1000];
char* visited[1000];
char** PNGs;
struct hsearch_data visitedHash;
CURLMsg* msg = NULL;

int frontierIndex = 0;
int urlsVisited = 0;
unsigned int totalPNGs = 0;
int pngsFound = 0;
int msgs_left = 0;
unsigned int connections = 0;
int stillRunning = 0;
CURLM* cm;

htmlDocPtr mem_getdoc(char *buf, int size, const char *url) {
    int opts = HTML_PARSE_NOBLANKS | HTML_PARSE_NOERROR | \
               HTML_PARSE_NOWARNING | HTML_PARSE_NONET;
    htmlDocPtr doc = htmlReadMemory(buf, size, url, NULL, opts);

    if ( doc == NULL ) {
        return NULL;
    }
    return doc;
}

xmlXPathObjectPtr getnodeset (xmlDocPtr doc, xmlChar *xpath) {

    xmlXPathContextPtr context;
    xmlXPathObjectPtr result;

    context = xmlXPathNewContext(doc);
    if (context == NULL) {
        printf("Error in xmlXPathNewContext\n");
        return NULL;
    }
    result = xmlXPathEvalExpression(xpath, context);
    xmlXPathFreeContext(context);
    if (result == NULL) {
        printf("Error in xmlXPathEvalExpression\n");
        return NULL;
    }
    if(xmlXPathNodeSetIsEmpty(result->nodesetval)){
        xmlXPathFreeObject(result);
        return NULL;
    }
    return result;
}

int find_http(char *buf, int size, int follow_relative_links, const char *base_url) {

    int i;
    htmlDocPtr doc;
    xmlChar *xpath = (xmlChar*) "//a/@href";
    xmlNodeSetPtr nodeset;
    xmlXPathObjectPtr result;
    xmlChar *href;

    ENTRY urlHash;
    ENTRY* urlFound;

    if (buf == NULL) {
        return 1;
    }

    doc = mem_getdoc(buf, size, base_url);
    result = getnodeset (doc, xpath);
    if (result) {
        nodeset = result->nodesetval;
        for (i=0; i < nodeset->nodeNr; i++) {
            href = xmlNodeListGetString(doc, nodeset->nodeTab[i]->xmlChildrenNode, 1);
            if ( follow_relative_links ) {
                xmlChar *old = href;
                href = xmlBuildURI(href, (xmlChar *) base_url);
                xmlFree(old);
            }
            if ( href != NULL && !strncmp((const char *)href, "http", 4) ) {
                /* If URL is not already visited, add to frontier and hash table. Post items. */
                urlHash.key = (char *) href;
                if (hsearch_r(urlHash, FIND, &urlFound, &visitedHash) == 0) {
                    ++frontierIndex;
                    frontier[frontierIndex] = malloc(strlen((char*)href)+1);
                    strcpy(frontier[frontierIndex],(char*)href);
                    urlHash.key = frontier[frontierIndex];

                    hsearch_r(urlHash, ENTER, &urlFound, &visitedHash);
                }
            }
            xmlFree(href);
        }
        xmlXPathFreeObject (result);
    }
    xmlFreeDoc(doc);
    return 0;
}

/**
 * @brief write callback function to save a copy of received data in RAM.
 *        The received libcurl data are pointed by p_recv,
 *        which is provided by libcurl and is not user allocated memory.
 *        The user allocated memory is at p_userdata. One needs to
 *        cast it to the proper struct to make good use of it.
 *        This function maybe invoked more than once by one invokation of
 *        curl_easy_perform().
 */

size_t write_cb_curl3(char *p_recv, size_t size, size_t nmemb, void *p_userdata) {
    size_t realsize = size * nmemb;
    RECV_BUF *p = (RECV_BUF *)p_userdata;

    if (p->size + realsize + 1 > p->max_size) {/* hope this rarely happens */
        /* received data is not 0 terminated, add one byte for terminating 0 */
        size_t new_size = p->max_size + max(BUF_INC, realsize + 1);
        char *q = realloc(p->buf, new_size);
        if (q == NULL) {
            perror("realloc"); /* out of memory */
            return -1;
        }
        p->buf = q;
        p->max_size = new_size;
    }

    memcpy(p->buf + p->size, p_recv, realsize); /*copy data from libcurl*/
    p->size += realsize;
    p->buf[p->size] = 0;

    return realsize;
}

int recv_buf_init(RECV_BUF *ptr, size_t max_size) {
    void *p = NULL;

    if (ptr == NULL) {
        return 1;
    }

    p = malloc(max_size);
    memset(p, 0, max_size);
    if (p == NULL) {
        return 2;
    }

    ptr->buf = p;
    ptr->size = 0;
    ptr->max_size = max_size;
    return 0;
}

int recv_buf_cleanup(RECV_BUF *ptr) {
    if (ptr == NULL) {
        return 1;
    }

    free(ptr->buf);
    ptr->size = 0;
    ptr->max_size = 0;
    free(ptr);
    return 0;
}

void cleanup(CURL *curl, RECV_BUF *ptr) {
    curl_easy_cleanup(curl);
    recv_buf_cleanup(ptr);
}
/**
 * @brief output data in memory to a file
 * @param path const char *, output file path
 * @param in  void *, input data to be written to the file
 * @param len size_t, length of the input data in bytes
 */

/**
 * @brief create a curl easy handle and set the options.
 * @param RECV_BUF *ptr points to user data needed by the curl write call back function
 * @param const char *url is the target url to fetch resoruce
 * @return a valid CURL * handle upon sucess; NULL otherwise
 * Note: the caller is responsbile for cleaning the returned curl handle
 */

CURL *easy_handle_init(RECV_BUF *ptr) {
    CURL *curl_handle = NULL;

    if ( ptr == NULL || ptr->url == NULL) {
        return NULL;
    }

    /* init user defined call back function buffer */
    if ( recv_buf_init(ptr, BUF_SIZE) != 0 ) {
        return NULL;
    }
    /* init a curl session */
    curl_handle = curl_easy_init();

    if (curl_handle == NULL) {
        fprintf(stderr, "curl_easy_init: returned NULL\n");
        return NULL;
    }

    /* specify URL to get */
    curl_easy_setopt(curl_handle, CURLOPT_URL, ptr->url);

    /* register write call back function to process received data */
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_cb_curl3);
    /* user defined data structure passed to the call back function */
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)ptr);

    /* some servers requires a user-agent field */
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "ece252 lab4 crawler");

    /* follow HTTP 3XX redirects */
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
    /* continue to send authentication credentials when following locations */
    curl_easy_setopt(curl_handle, CURLOPT_UNRESTRICTED_AUTH, 1L);
    /* max number of redirects to follow sets to 5 */
    curl_easy_setopt(curl_handle, CURLOPT_MAXREDIRS, 5L);
    /* supports all built-in encodings */
    curl_easy_setopt(curl_handle, CURLOPT_ACCEPT_ENCODING, "");

    /* Enable the cookie engine without reading any initial cookies */
    curl_easy_setopt(curl_handle, CURLOPT_COOKIEFILE, "");
    /* allow whatever auth the proxy speaks */
    curl_easy_setopt(curl_handle, CURLOPT_PROXYAUTH, CURLAUTH_ANY);
    /* allow whatever auth the server speaks */
    curl_easy_setopt(curl_handle, CURLOPT_HTTPAUTH, CURLAUTH_ANY);

    /* Associate our buf pointer with this curl header. */
    curl_easy_setopt(curl_handle, CURLOPT_PRIVATE, (void *)ptr);

    return curl_handle;
}

int process_html(CURL *curl_handle, RECV_BUF *p_recv_buf) {
    int follow_relative_link = 1;
    char *url = NULL;

    curl_easy_getinfo(curl_handle, CURLINFO_EFFECTIVE_URL, &url);
    find_http(p_recv_buf->buf, p_recv_buf->size, follow_relative_link, url);
    return 0;
}

/**
 * @brief process teh download data by curl
 * @param CURL *curl_handle is the curl handler
 * @param RECV_BUF p_recv_buf contains the received data.
 * @return 0 on success; non-zero otherwise
 */

void process_data(CURL *curl_handle, RECV_BUF *p_recv_buf) {
    CURLcode res;
    long response_code;

    res = curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &response_code);
    if ( response_code >= 400 ) {
        return;
    }

    char *ct = NULL;
    res = curl_easy_getinfo(curl_handle, CURLINFO_CONTENT_TYPE, &ct);
    if ( res == CURLE_OK && ct != NULL ) {
    } else {
        return;
    }

    if ( strstr(ct, CT_HTML) ) {
        process_html(curl_handle, p_recv_buf);
        return;
    } else if ( strstr(ct, CT_PNG) ) {
        return;
    }
}

void watcher() {
    CURL* eh;
    RECV_BUF* recv_buf;
    while (1) {
        /* Add urls from frontier to the curl_multi to be crawled. */
        if ((frontierIndex > -1) && (stillRunning < connections) && (pngsFound != totalPNGs)) {
            visited[urlsVisited] = frontier[frontierIndex];
            --frontierIndex;
            ++urlsVisited;

            recv_buf = malloc(sizeof(RECV_BUF));
            recv_buf->url = visited[urlsVisited-1];
            eh = easy_handle_init(recv_buf);
            curl_multi_add_handle(cm, eh);
        }
        else if (stillRunning == 0) {
            /* If frontier is empty and nothing is running. Done. */
            break;
        }
        curl_multi_perform(cm, &stillRunning);

        /* Check if any headers are done. */
        while ((msg = curl_multi_info_read(cm, &msgs_left))) {
            if (msg->msg == CURLMSG_DONE) {
                eh = msg->easy_handle;
                curl_easy_getinfo(eh, CURLINFO_PRIVATE, &recv_buf);

                /* If received data is png, add url to array if needed. */
                if (pngsFound < totalPNGs) {
                    if (recv_buf->buf[0] == '\211' && recv_buf->buf[1] == 'P' && recv_buf->buf[2] == 'N' && recv_buf->buf[3] == 'G' && recv_buf->buf[4] == '\r' && recv_buf->buf[5] == '\n' && recv_buf->buf[6] == '\032' && recv_buf->buf[7] == '\n') {
                        PNGs[pngsFound] = recv_buf->url;
                        ++pngsFound;
                    }
                }
                else {
                    curl_multi_remove_handle(cm, eh);
                    cleanup(eh, recv_buf);
                    continue;
                }

                /* Process received data. */
                process_data(eh, recv_buf);

                /* Cleanup. */
                curl_multi_remove_handle(cm, eh);
                cleanup(eh, recv_buf);
            }
        }
    }
}

int main(int argc, char *argv[]) {
	
	/* Timer Start. */
	double times[2];
    struct timeval tv;
	
	if (gettimeofday(&tv, NULL) != 0) {
        perror("gettimeofday");
        abort();
    }
    times[0] = (tv.tv_sec) + tv.tv_usec/1000000.;

    int c;                  /* For argument parsing. */
    unsigned int t = 1;     /* Number of concurrent connections. */
    unsigned int m = 50;    /* Number of PNGs to find. */
    char* v = NULL;         /* file name for visitedHash URLs. */
    char* SEED_URL = NULL;  /* Starting URL. */

    /* Parsing command line arguments. */
    while ((c = getopt(argc, argv, "t:m:v:")) != -1) {
        switch (c) {
            case 't':
                t = strtoul(optarg, NULL, 10);
                break;
            case 'm':
                m = strtoul(optarg, NULL, 10);
                break;
            case 'v':
                v = malloc(strlen(optarg) + 1);
                strcpy(v, optarg);
                logURLs = true;
                break;
            default:
                return -1;
        }
    }
    SEED_URL = malloc(strlen(argv[optind]) + 1);
    strcpy(SEED_URL, argv[optind]);

    /* Init global variables. */
    frontier[0] = SEED_URL;
    connections = t;
    totalPNGs = m;
    PNGs = malloc(m * sizeof(char *));
    memset((void *)&visitedHash, 0, sizeof(visitedHash));
    hcreate_r(1000, &visitedHash);

    /* Add initial url to hash table. */
    ENTRY urlHash;
    ENTRY* urlFound;
    urlHash.key = SEED_URL;
    hsearch_r(urlHash, ENTER, &urlFound, &visitedHash);

    /* cURL part. */
    curl_global_init(CURL_GLOBAL_ALL);
    cm = curl_multi_init();
    watcher();

    /* Output found PNG urls. */
    FILE* visitedPNGs = fopen("png_urls.txt", "w");
    for (int i = 0; i < pngsFound; ++i) {
        fputs(PNGs[i], visitedPNGs);
        fputs("\n", visitedPNGs);
    }
    fclose(visitedPNGs);

    /* Output all visited URLs. */
    if (logURLs) {
        FILE* visitedFile = fopen(v, "w");
        for (int i = 0; i < urlsVisited; ++i) {
            fputs(visited[i], visitedFile);
            fputs("\n", visitedFile);
            free(visited[i]);
        }
        fclose(visitedFile);
    }
    else {
        for (int i = 0; i < urlsVisited; ++i) {
            free(visited[i]);
        }
    }

    /* Cleanup. */
    for (int i = 0; i <= frontierIndex; ++i) {
        free(frontier[i]);
    }
    free(v);
    free(PNGs);
    hdestroy_r(&visitedHash);
    curl_multi_cleanup(cm);
    curl_global_cleanup();
	
	/* Timer End. */
	if (gettimeofday(&tv, NULL) != 0) {
        perror("gettimeofday");
        abort();
    }
    times[1] = (tv.tv_sec) + tv.tv_usec/1000000.;
    printf("findpng3 execution time: %.6lf seconds\n", times[1] - times[0]);

    return 0;
}