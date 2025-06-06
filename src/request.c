#include "io_helper.h"
#include "request.h"

#define MAXBUF (8192)

// below default values are defined in 'request.h'
int num_threads = DEFAULT_THREADS;
int buffer_max_size = DEFAULT_BUFFER_SIZE;
int scheduling_algo = DEFAULT_SCHED_ALGO;	

//
//	TODO: add code to create and manage the shared global buffer of requests
//	HINT: You will need synchronization primitives.
//		pthread_mutuex_t lock_var is a viable option.
//
int timespec_compare(struct timespec *a, struct timespec *b) {
    if (a->tv_sec < b->tv_sec) return -1;
    if (a->tv_sec > b->tv_sec) return 1;
    if (a->tv_nsec < b->tv_nsec) return -1;
    if (a->tv_nsec > b->tv_nsec) return 1;
    return 0;
}
typedef struct {
    int conn_fd;      // connection file descriptor
    char filename[MAXBUF];  // requested filename
    int filesize;     // file size (optional - can be used for SFF later)
    int usage_count;       // for starvation prevention (optional, for SFF)
    struct timespec arrival_time;  // timestamp for FIFO tiebreak
} request_t;

request_t* buffer;
int buffer_size = 0;
int buffer_head = 0;
int buffer_tail = 0;

// synchronization primitives
pthread_mutex_t buffer_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t buffer_not_empty = PTHREAD_COND_INITIALIZER;
pthread_cond_t buffer_not_full = PTHREAD_COND_INITIALIZER;

// Initialize buffer at start 
void request_buffer_init() {
    buffer = malloc(sizeof(request_t) * buffer_max_size);
    assert(buffer != NULL);
}

// Insert request into buffer
void request_buffer_insert(int conn_fd, char *filename, int filesize) {
    pthread_mutex_lock(&buffer_lock);
    
    while (buffer_size == buffer_max_size)
        pthread_cond_wait(&buffer_not_full, &buffer_lock);

    buffer[buffer_tail].conn_fd = conn_fd;
    strcpy(buffer[buffer_tail].filename, filename);
    buffer[buffer_tail].filesize = filesize;
    buffer[buffer_tail].usage_count = 0;
    clock_gettime(CLOCK_REALTIME, &buffer[buffer_tail].arrival_time);

    buffer_tail = (buffer_tail + 1) % buffer_max_size;
    buffer_size++;

    pthread_cond_signal(&buffer_not_empty);
    pthread_mutex_unlock(&buffer_lock);
}
// Remove request from buffer
request_t request_buffer_remove() {
    pthread_mutex_lock(&buffer_lock);

    while (buffer_size == 0)
        pthread_cond_wait(&buffer_not_empty, &buffer_lock);

    int idx = buffer_head;  // default FIFO

    if (scheduling_algo == 1) { // SFF
        for (int i = 0; i < buffer_size; i++) {
            int real_idx = (buffer_head + i) % buffer_max_size;
            if (buffer[real_idx].filesize < buffer[idx].filesize ||
                (buffer[real_idx].filesize == buffer[idx].filesize &&
                 timespec_compare(&buffer[real_idx].arrival_time, &buffer[idx].arrival_time) < 0)) {
                idx = real_idx;
            }
        }
    } else if (scheduling_algo == 2) { // RANDOM
        int random_offset = rand() % buffer_size;
        idx = (buffer_head + random_offset) % buffer_max_size;
    }

    request_t req = buffer[idx];

    // Move last element into idx to keep circular buffer tight
    int last_idx = (buffer_tail - 1 + buffer_max_size) % buffer_max_size;
    buffer[idx] = buffer[last_idx];
    buffer_tail = last_idx;
    buffer_size--;

    pthread_cond_signal(&buffer_not_full);
    pthread_mutex_unlock(&buffer_lock);

    return req;
}
//
// Sends out HTTP response in case of errors
//
void request_error(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
    char buf[MAXBUF], body[MAXBUF];
    
    // Create the body of error message first (have to know its length for header)
    sprintf(body, ""
	    "<!doctype html>\r\n"
	    "<head>\r\n"
	    "  <title>CYB-3053 WebServer Error</title>\r\n"
	    "</head>\r\n"
	    "<body>\r\n"
	    "  <h2>%s: %s</h2>\r\n" 
	    "  <p>%s: %s</p>\r\n"
	    "</body>\r\n"
	    "</html>\r\n", errnum, shortmsg, longmsg, cause);
    
    // Write out the header information for this response
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    write_or_die(fd, buf, strlen(buf));
    
    sprintf(buf, "Content-Type: text/html\r\n");
    write_or_die(fd, buf, strlen(buf));
    
    sprintf(buf, "Content-Length: %lu\r\n\r\n", strlen(body));
    write_or_die(fd, buf, strlen(buf));
    
    // Write out the body last
    write_or_die(fd, body, strlen(body));
    
    // close the socket connection
    // done later now close_or_die(fd);
}

//
// Reads and discards everything up to an empty text line
//
void request_read_headers(int fd) {
    char buf[MAXBUF];
    
    readline_or_die(fd, buf, MAXBUF);
    while (strcmp(buf, "\r\n")) {
	readline_or_die(fd, buf, MAXBUF);
    }
    return;
}

//
// Return 1 if static, 0 if dynamic content (executable file)
// Calculates filename (and cgiargs, for dynamic) from uri
int request_parse_uri(char *uri, char *filename, char *cgiargs) {
    char *ptr;
    
    if (!strstr(uri, "cgi")) { 
	// static
	strcpy(cgiargs, "");
	sprintf(filename, ".%s", uri);
	if (uri[strlen(uri)-1] == '/') {
	    strcat(filename, "index.html");
	}
	return 1;
    } else { 
	// dynamic
	ptr = index(uri, '?');
	if (ptr) {
	    strcpy(cgiargs, ptr+1);
	    *ptr = '\0';
	} else {
	    strcpy(cgiargs, "");
	}
	sprintf(filename, ".%s", uri);
	return 0;
    }
}

//
// Fills in the filetype given the filename
//
void request_get_filetype(char *filename, char *filetype) {
    if (strstr(filename, ".html")) 
	strcpy(filetype, "text/html");
    else if (strstr(filename, ".gif")) 
	strcpy(filetype, "image/gif");
    else if (strstr(filename, ".jpg")) 
	strcpy(filetype, "image/jpeg");
    else 
	strcpy(filetype, "text/plain");
}

//
// Handles requests for static content
//
void request_serve_static(int fd, char *filename, int filesize) {
    int srcfd;
    char *srcp, filetype[MAXBUF], buf[MAXBUF];
    
    request_get_filetype(filename, filetype);
    srcfd = open_or_die(filename, O_RDONLY, 0);
    
    // Rather than call read() to read the file into memory, 
    // which would require that we allocate a buffer, we memory-map the file
    srcp = mmap_or_die(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
    close_or_die(srcfd);
    
    // put together response
    sprintf(buf, ""
	    "HTTP/1.0 200 OK\r\n"
	    "Server: OSTEP WebServer\r\n"
	    "Content-Length: %d\r\n"
	    "Content-Type: %s\r\n\r\n", 
	    filesize, filetype);
       
    write_or_die(fd, buf, strlen(buf));
    
    //  Writes out to the client socket the memory-mapped file 
    write_or_die(fd, srcp, filesize);
    munmap_or_die(srcp, filesize);
}

//
// Fetches the requests from the buffer and handles them (thread logic)
//
void* thread_request_serve_static(void* arg) {
    while (1) {
        request_t req = request_buffer_remove();

        struct stat sbuf;
        if (stat(req.filename, &sbuf) < 0) {
            request_error(req.conn_fd, req.filename, "404", "Not Found", "File not found");
            close_or_die(req.conn_fd);
            continue;
        }

        // serve the file
        request_serve_static(req.conn_fd, req.filename, sbuf.st_size);

        // close connection after serving
        close_or_die(req.conn_fd);
    }
    return NULL;
}
//
// Initial handling of the request
//
void request_handle(int fd) {
    int is_static;
    struct stat sbuf;
    char buf[MAXBUF], method[MAXBUF], uri[MAXBUF], version[MAXBUF];
    char filename[MAXBUF], cgiargs[MAXBUF];
    
    // get the request type, file path and HTTP version
    readline_or_die(fd, buf, MAXBUF);
    sscanf(buf, "%s %s %s", method, uri, version);
    printf("method:%s uri:%s version:%s\n", method, uri, version);

    // verify if the request type is GET or not
    if (strcasecmp(method, "GET")) {
	request_error(fd, method, "501", "Not Implemented", "server does not implement this method");
	return;
    }
    request_read_headers(fd);
    
    // check requested content type (static/dynamic)
    is_static = request_parse_uri(uri, filename, cgiargs);
    
    // get some data regarding the requested file, also check if requested file is present on server
    if (stat(filename, &sbuf) < 0) {
	request_error(fd, filename, "404", "Not found", "server could not find this file");
	return;
    }
    
    // verify if requested content is static
    if (is_static) {
	if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {
		request_error(fd, filename, "403", "Forbidden", "server could not read this file");
		return;
	}
    
	// TODO: directory traversal mitigation	
    if (strstr(filename, "..") != NULL) {
        request_error(fd, filename, "403", "Forbidden", "directory traversal attempt detected");
        close_or_die(fd);
        return;
    }
	// TODO: write code to add HTTP requests in the buffer
    // Add request to global buffer
    request_buffer_insert(fd, filename, sbuf.st_size);
    return;
    } else {
	request_error(fd, filename, "501", "Not Implemented", "server does not serve dynamic content request");
    }
}
