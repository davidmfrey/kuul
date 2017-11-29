#include "kuhli.h"
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <uv.h>
#include <curl/curl.h>
#include <stdio.h>

struct kuhli_global_t {
  pthread_t thread;
  uv_loop_t loop;
  CURLM *multi;
  int counter;
  kuhli_t *kuhli_list;
  union {
    uv_handle_t curl_timer_h;
    uv_timer_t curl_timer;
  };
  union {
    uv_handle_t inject_h;
    uv_poll_t inject;
  };
  int input;
  int output;
};

typedef enum { TASK_START,
	       TASK_WRITE,
	       TASK_END,
	       TASK_SHUTDOWN } TASK_TYPE;

typedef struct kuhli_task_s {
  TASK_TYPE type;
  kuhli_t *k;
  char *data;
  int data_len;
} kuhli_task_t;

typedef struct kuhli_buf_s {
  char *buf;
  int len;
  int max;
} kuhli_buf_t;

typedef struct kuhli_data_s {
  char *start;
  char *rp;
  char *ep;
  kuhli_t *k;
  struct kuhli_data_s *next;
} kuhli_data_t;

typedef struct kuhli_header_s {
  char *key;
  char *value;
  struct kuhli_header_s *next;
} kuhli_header_t;

struct kuhli_t {
  kuhli_global_t *g;
  kuhli_t *next;
  CURL *easy;
  kuhli_buf_t *querybuf;
  kuhli_buf_t *formbuf;
  kuhli_buf_t *buf;
  int chunked;
  int body_length;
  int bytes_written;
  kuhli_data_t *body;
  kuhli_data_t *tail;
  char curl_error_buf[CURL_ERROR_SIZE];
  
  void *opaque;
  kuhli_complete_cb on_complete;
  kuhli_headers_cb on_headers;
  kuhli_body_chunk_cb on_body_chunk;
  
  KUHLI_PROTOCOL protocol;
  KUHLI_METHOD method;
  char *host;
  char *path;
  int port;
  struct curl_slist *out_headers;
  kuhli_header_t *in_headers;
  int verbose;
  int user_ended;
  int paused;
  int started;
  int finished;

  FILE *file;
};

typedef struct kuhli_curl_socket_context_s {
  kuhli_global_t *g;
  union { 
    uv_handle_t poll_h; 
    uv_poll_t poll;
  };
  int fd;
} kuhli_curl_socket_context;

static kuhli_buf_t *kuhli_buf_init( void ) {
  kuhli_buf_t *b = calloc(sizeof(*b), 1);
  return b;
}

static void kuhli_buf_destroy( kuhli_buf_t *b ) {
  if(b) {
    free(b->buf);
    free(b);
  }
}

static void kuhli_buf_try_grow( kuhli_buf_t *b, int len ) {
  if(!b->max) {
    b->max = 8;
  }
  if(b->max - b->len <= len) {
    while(b->max - b->len <= len) {
      b->max *= 2;
    }
    b->buf = realloc(b->buf, b->max);
  }
}

static void kuhli_buf_appendf( kuhli_buf_t *b, char const *fmt, ... ) {
  va_list args;
  va_list args2;
  va_start(args, fmt);
  va_copy(args2, args);
  int len = vsnprintf(NULL, 0, fmt, args);
  va_end(args);
  kuhli_buf_try_grow(b, len);
  vsprintf(b->buf+b->len, fmt, args2);
  va_end(args2);
  b->len += len;
  b->buf[b->len] = 0;
}

static void kuhli_uv_free_data( uv_handle_t *h ) {
  free(h->data);
}

static void kuhli_add_data_chunk( kuhli_t *k, char const *data, size_t len, int copy ) {
  if(len && data) {
    kuhli_data_t *d = calloc(sizeof(*d), 1);
    if(copy) {
      d->rp = d->start = malloc(len);
      memcpy(d->start, data, len);
    }
    else {
      d->rp = d->start = (char *)data;
    }
    d->ep = d->start+len;
    d->k = k;
    if(!k->body) {
      k->body = k->tail = d;
    }
    else {
      k->tail->next = d;
      k->tail = d;
    }
  }
}

static kuhli_t *kuhli_remove_from_list( kuhli_global_t *g, CURL *easy ) {
  kuhli_t *res = g->kuhli_list;
  if(g->kuhli_list->easy == easy) {
    g->kuhli_list = g->kuhli_list->next;
  }
  else {
    kuhli_t *tmp;
    for(tmp = g->kuhli_list; tmp->next->easy != easy; tmp = tmp->next);
    res = tmp->next;
    tmp->next = tmp->next->next;
  }
  return res;
}

static void kuhli_destroy( kuhli_t *k ) {
  if(k->user_ended && k->finished) {
    kuhli_buf_destroy(k->querybuf);
    kuhli_buf_destroy(k->formbuf);
    kuhli_buf_destroy(k->buf);
    free(k->path);
    free(k->host);
    while(k->in_headers) {
      void *m = k->in_headers;
      k->in_headers = k->in_headers->next;
      free(m);
    }
    curl_easy_cleanup(k->easy);
    free(k);
  }
}

static void kuhli_clean_up_finished( kuhli_global_t *g ) {
  /*  see if any easy handles are complete and wrap them up if so  */
  CURLMsg* message = NULL;
  CURL* easy = NULL;
  int pending = 0;
  while((message = curl_multi_info_read(g->multi, &pending ))) {
    if(message->msg == CURLMSG_DONE) {
      easy = message->easy_handle;
      curl_multi_remove_handle(g->multi, easy);
      kuhli_t *k = kuhli_remove_from_list(g, easy);
      long status;
      curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
      if(k->on_complete) {
	k->on_complete(k, status, k->opaque);
      }
      //fprintf(stderr, "Curl status: %ld\n", status);
      //fprintf(stderr, "Curl Err: %s\n", k->curl_error_buf);
      k->finished = 1;
      kuhli_destroy(k);
    }
  }
}

static void kuhli_loop_uv_injection( uv_poll_t *p, int status, int events ) {
  /*  somebody outside wants something done!  */
  kuhli_global_t *g = p->data;
  kuhli_task_t *tasks[32];
  int num_read = read(g->output, tasks, sizeof(kuhli_task_t *)*32);
  if(num_read) {
    num_read /= sizeof(kuhli_task_t *);
    int i;
    for(i=0; i<num_read; i++) {
      kuhli_task_t *t = tasks[i];
      kuhli_t *k = t->k;
      switch(t->type) {
      case TASK_START:
	curl_multi_add_handle(g->multi, k->easy);
	break;
      case TASK_WRITE:
	if(!k->finished) {
	  kuhli_add_data_chunk(k, t->data, t->data_len, 0);
	  if(k->paused) {
	    k->paused = 0;
	    curl_easy_pause(k->easy, CURLPAUSE_CONT);
#ifdef USE_CURL_MULTISOCKET_ALL
	    curl_multi_socket_all(g->multi, &g->counter);
#else
	    curl_multi_socket_action(g->multi, CURL_SOCKET_TIMEOUT, 0, &g->counter);
#endif
	    kuhli_clean_up_finished(g);
	  }
	}
	break;
      case TASK_END:
	k->user_ended = 1;
	if(!k->finished) {
	  if(k->paused) {
	    k->paused = 0;
	    curl_easy_pause(k->easy, CURLPAUSE_CONT);
#ifdef USE_CURL_MULTISOCKET_ALL
	    curl_multi_socket_all(g->multi, &g->counter);
#else
	    curl_multi_socket_action(g->multi, CURL_SOCKET_TIMEOUT, 0, &g->counter);
#endif
	    kuhli_clean_up_finished(g);
	  }
	}
	else {
	  kuhli_destroy(k);
	}
	break;
      case TASK_SHUTDOWN:
	break;
      }
      free(t);
    }
  }
}

static kuhli_curl_socket_context* kuhli_init_curl_socket_context( kuhli_global_t * g, curl_socket_t s, CURL* easy) {
  kuhli_curl_socket_context * c = calloc(sizeof(*c), 1);
  c->g = g;
  c->fd = s;
  uv_poll_init(&g->loop, &c->poll, c->fd);
  c->poll.data = c;
  curl_multi_assign(g->multi, s, c);
  return c;
}

static void kuhli_destroy_curl_socket_context( kuhli_curl_socket_context *c ) {
  uv_close(&c->poll_h, kuhli_uv_free_data);
}

static void kuhli_loop_uv_socket_event( uv_poll_t* p, int status, int events ) {
  /*  libuv says we have some sweet socket action; relay to libcurl  */
  kuhli_curl_socket_context *c = p->data;
  kuhli_global_t* g = c->g;
  int flags = ((events & UV_READABLE) ? CURL_CSELECT_IN : 0)|((events & UV_WRITABLE) ? CURL_CSELECT_OUT : 0);
  while(curl_multi_socket_action(g->multi, c->fd, flags, &g->counter) == CURLM_CALL_MULTI_PERFORM);
  kuhli_clean_up_finished(g);
}

static int kuhli_loop_curl_socket( CURL *easy, curl_socket_t s, int action, void *opaque, void *socket_opaque ) {
  /*  libcurl wants to register/deregister events on socket s; set it up with libuv  */
  kuhli_global_t* g = opaque;
  kuhli_curl_socket_context* context = NULL;
  if (!(action & CURL_POLL_REMOVE)) {
    context = socket_opaque ? socket_opaque : kuhli_init_curl_socket_context(g, s, easy);
    int flags = ((action & CURL_POLL_IN) ? UV_READABLE : 0)|((action & CURL_POLL_OUT) ? UV_WRITABLE : 0);
    uv_poll_start(&context->poll, flags, kuhli_loop_uv_socket_event);
  }
  else if((context = socket_opaque)) {
    uv_poll_stop( &context->poll );
    kuhli_destroy_curl_socket_context(context);
  }
  return 0;
}

static void kuhli_loop_uv_timeout( uv_timer_t *t ) {
  /*  libuv says we have a timeout; relay to libcurl  */
  kuhli_global_t * g = t->data;
  while (curl_multi_socket_action(g->multi, CURL_SOCKET_TIMEOUT, 0, &g->counter) == CURLM_CALL_MULTI_PERFORM);
  kuhli_clean_up_finished(g);
}

static int kuhli_loop_curl_set_timer(CURLM* multi, long timeout, void *opaque) {
  /*  libcurl wants to set a timeout; set it up with libuv  */
  kuhli_global_t *g = opaque;
  if(timeout == 0) {
    curl_multi_socket_action(g->multi, CURL_SOCKET_TIMEOUT, 0, &g->counter);
    kuhli_clean_up_finished(g);
  }
  else if(timeout == -1) {
    if(uv_is_active(&g->curl_timer_h)) {
      uv_timer_stop(&g->curl_timer);
    }
  }
  else {
    uv_timer_start(&g->curl_timer, kuhli_loop_uv_timeout, timeout, 0 );
  }
  return 0;
}

static size_t kuhli_loop_curl_header( char *buf, size_t size, size_t num, void *opaque ) {
  kuhli_t *k = opaque;
  size_t res = size*num;
  kuhli_header_t *h = malloc(sizeof(*h)+res+1);
  h->key = h->value = (char *)(h+1);
  memcpy(h->key, buf, res);
  h->key[res] = 0;
  h->next = k->in_headers;
  k->in_headers = h;
  return res;
}

static size_t kuhli_loop_curl_write_body( char *buf, size_t size, size_t num, void *opaque ) {
  size_t res = size*num;
  kuhli_t *k = opaque;
  if(k->on_body_chunk) {
    k->on_body_chunk(k, buf, res, k->opaque);
  }
  else {
    for(size_t i=0; i<res; i++) {
      putchar(buf[i]);
    }
  }
  return res;
}

static size_t kuhli_loop_curl_read_body( char *buf, size_t size, size_t num, void *opaque ) {
  kuhli_t *k = opaque;
  size_t available_bytes = size*num;
  size_t total_written = 0;
  while(available_bytes && k->body) {
    int64_t bytes_to_write = k->body->ep-k->body->rp;
    int64_t written = (available_bytes >= bytes_to_write) ? bytes_to_write : available_bytes;
    memcpy(buf+total_written, k->body->rp, written);
    k->body->rp += written;
    available_bytes -= written;
    total_written += written;
    if(k->body->rp == k->body->ep) {
      kuhli_data_t *tmp = k->body;
      k->body = k->body->next;
      free(tmp->start);
      free(tmp);
    }
  }
  if(total_written) {
    fwrite(buf, 1, total_written, k->file);
  }
  if(!k->body) {
    k->tail = NULL;
  }
  k->bytes_written += total_written;
  if(total_written) {
    return total_written;
  }
  if(!k->chunked) {
    if(k->bytes_written == k->body_length) {
      return 0;
    }
    k->paused = 1;
    return CURL_READFUNC_PAUSE;
  }
  /*  chunk-encoded body  */
  if(k->user_ended) {
    return 0;
  }
  k->paused = 1;
  return CURL_READFUNC_PAUSE;
}

static void *kuhli_uv_thread( void *arg ) {
  /*  spin up a thread and run a loop until it exits  */
  kuhli_global_t *g = arg;
  uv_run(&g->loop, UV_RUN_DEFAULT);
  while(uv_loop_close(&g->loop) == UV_EBUSY);
  return NULL;
}

void kuhli_opaque( kuhli_t *k, void *opaque ) {
  k->opaque = opaque;
}

void kuhli_verbose( kuhli_t *k, int v ) {
  k->verbose = v;
}

void kuhli_on_body_chunk( kuhli_t *k, kuhli_body_chunk_cb cb ) {
  k->on_body_chunk = cb;
}

void kuhli_on_complete( kuhli_t *k, kuhli_complete_cb cb ) {
  k->on_complete = cb;
}

kuhli_global_t *kuhli_global_init( void ) {
  curl_global_init(CURL_GLOBAL_ALL);
  kuhli_global_t *g = calloc(sizeof(*g), 1);
  uv_loop_init(&g->loop);
  uv_timer_init(&g->loop, &g->curl_timer);
  g->curl_timer.data = g;
  
  int fds[2];
  int n = pipe(fds);
  (void) n;
  g->output = fds[0];
  g->input = fds[1];
  uv_poll_init(&g->loop, &g->inject, g->output);
  g->inject.data = g;
  uv_poll_start(&g->inject, UV_READABLE, kuhli_loop_uv_injection);
  
  g->multi = curl_multi_init();
  curl_multi_setopt(g->multi, CURLMOPT_SOCKETFUNCTION, kuhli_loop_curl_socket);
  curl_multi_setopt(g->multi, CURLMOPT_SOCKETDATA, g);
  curl_multi_setopt(g->multi, CURLMOPT_TIMERFUNCTION, kuhli_loop_curl_set_timer);
  curl_multi_setopt(g->multi, CURLMOPT_TIMERDATA, g);
  
  pthread_create(&g->thread, NULL, kuhli_uv_thread, g);
  return g;
}

void kuhli_global_cleanup( kuhli_global_t *g ) {
  
}

void kuhli_global_cleanup_async( kuhli_global_t *g ) {
  
}

static kuhli_task_t *kuhli_init_task( kuhli_t *k, TASK_TYPE type ) {
  kuhli_task_t *t = calloc(sizeof(*t), 1);
  t->type = type;
  t->k = k;
  return t;
}

static void kuhli_add_task( kuhli_task_t *t ) {
  write(t->k->g->input, &t, sizeof(t));
}

static void kuhli_start_request( kuhli_t *k ) {
  if(!k->started) {
    k->started = 1;
    k->easy = curl_easy_init();
    if(k->port < 0) {
      k->port = (k->protocol == KUHLI_HTTP) ? 80 : 443;
    }
    if(!k->path) {
      k->path = strdup("/");
    }
    kuhli_buf_appendf(k->buf, "%s://%s:%d%s", 
		(k->protocol == KUHLI_HTTP) ? "http" : "https", k->host, k->port, k->path);
    if(k->querybuf) {
      kuhli_buf_appendf(k->buf, "?%s", k->querybuf->buf);
    }
    curl_easy_setopt(k->easy, CURLOPT_PRIVATE, k);
    if(k->verbose) {
      curl_easy_setopt(k->easy, CURLOPT_VERBOSE, 1L);
    }
    curl_easy_setopt(k->easy, CURLOPT_URL, k->buf->buf);
    curl_easy_setopt(k->easy, CURLOPT_ERRORBUFFER, k->curl_error_buf);
    curl_easy_setopt(k->easy, CURLOPT_HEADERFUNCTION, kuhli_loop_curl_header);
    curl_easy_setopt(k->easy, CURLOPT_HEADERDATA, k);
    curl_easy_setopt(k->easy, CURLOPT_WRITEFUNCTION, kuhli_loop_curl_write_body);
    curl_easy_setopt(k->easy, CURLOPT_WRITEDATA, k);
    switch(k->method) {
    case KUHLI_GET:
      curl_easy_setopt(k->easy, CURLOPT_HTTPGET, 1L);
      break;
    case KUHLI_PUT:
      curl_easy_setopt(k->easy, CURLOPT_UPLOAD, 1L);
      break;
    case KUHLI_POST:
      curl_easy_setopt(k->easy, CURLOPT_POST, 1L);
      break;
    case KUHLI_DELETE:
      curl_easy_setopt(k->easy, CURLOPT_CUSTOMREQUEST, "DELETE");
      break;
    }
    if(k->chunked || k->body) {
      curl_easy_setopt(k->easy, CURLOPT_READFUNCTION, kuhli_loop_curl_read_body);
      curl_easy_setopt(k->easy, CURLOPT_READDATA, k);
      if(k->chunked) {
	kuhli_header(k, "Transfer-Encoding", "chunked");
      }
      else {
	char buf[64];
	snprintf(buf, 64, "%d", k->body_length);
	kuhli_header(k, "Content-Length", buf);
      }
    }
    if(k->out_headers) {
      curl_easy_setopt(k->easy, CURLOPT_HTTPHEADER, k->out_headers );
    }
    kuhli_add_task(kuhli_init_task(k, TASK_START));
  }
}

kuhli_t *kuhli_init( kuhli_global_t *g, KUHLI_METHOD m ) {
  kuhli_t *k = calloc(sizeof(*k), 1);
  k->method = m;
  k->port = -1;
  k->body_length = -1;
  k->g = g;
  k->buf = kuhli_buf_init();
  k->protocol = KUHLI_HTTP;
  k->next = g->kuhli_list;
  g->kuhli_list = k;
  k->file = fopen("./reqOut.bin", "w");
  return k;
}

void kuhli_protocol( kuhli_t *k, KUHLI_PROTOCOL p ) {
  k->protocol = p;
}

void kuhli_host( kuhli_t *k, char const *host ) {
  k->host = strdup(host);
}

void kuhli_port( kuhli_t *k, int port ) {
  k->port = port;
}

void kuhli_path( kuhli_t *k, char const *path ) {
  k->path = strdup(path);
}

void kuhli_append( kuhli_t *k, char const *key, char const *value ) {
  switch(k->method) {
  case KUHLI_GET:
  case KUHLI_DELETE:
    return kuhli_append_query(k, key, value);
  default:
    return kuhli_append_form(k, key, value);
  }
}

static void kuhli_append_param( kuhli_t *k, kuhli_buf_t *buf, char const *key, char const *value ) {
  key = curl_easy_escape(k->easy, key, 0);
  value = curl_easy_escape(k->easy, value, 0);
  if(buf->len) {
    kuhli_buf_appendf(buf, "&%s=%s", key, value);
  }
  else {
    kuhli_buf_appendf(buf, "%s=%s", key, value);
  }
}

void kuhli_append_query( kuhli_t *k, char const *key, char const *value ) {
  if(key && key[0] && value) {
    if(!k->querybuf) {
      k->querybuf = kuhli_buf_init();
    }
    kuhli_append_param(k, k->querybuf, key, value);
  }
}

void kuhli_append_form( kuhli_t *k, char const *key, char const *value ) {
  if(key && key[0] && value) {
    if(!k->formbuf) {
      k->formbuf = kuhli_buf_init();
    }
    kuhli_append_param(k, k->formbuf, key, value);
  }
}

void kuhli_header( kuhli_t *k, char const *key, char const *value ) {
  if(key && key[0] && value) {
    char buf[1024];
    snprintf(buf, 1024, "%s: %s", key, value);
    k->out_headers = curl_slist_append(k->out_headers, buf);
  }
}

int kuhli_write( kuhli_t *k, char const *data, size_t len ) {
  if(k->finished) {
    return 0;
  }
  if(!k->started) {
    k->chunked = 1;
    kuhli_add_data_chunk(k, data, len, 1);
    kuhli_start_request(k);
  }
  else {
    kuhli_task_t *t = kuhli_init_task(k, TASK_WRITE);
    t->data = malloc(len);
    t->data_len = len;
    memcpy(t->data, data, len);
    kuhli_add_task(t);
  }
  return 1;
}

int kuhli_write_end( kuhli_t *k, char const *data, size_t len ) {
  if(!k->started) {
    k->body_length = len;
    kuhli_add_data_chunk(k, data, len, 1);
    kuhli_start_request(k);
  }
  else {
    kuhli_write(k, data, len);
    kuhli_end(k);
  }
  return 1;
}

void kuhli_end( kuhli_t *k ) {
  if(!k->started) {
    if(k->formbuf) {
      k->body_length = k->formbuf->len;
      kuhli_add_data_chunk(k, k->formbuf->buf, k->body_length, 1);
    }
    kuhli_start_request(k);
  }
  kuhli_add_task(kuhli_init_task(k, TASK_END));
}
