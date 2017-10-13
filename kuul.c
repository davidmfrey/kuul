#include "kuul.h"
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <uv.h>
#include <curl/curl.h>


struct kuul_global_s {
  pthread_t thread;
  uv_loop_t loop;
  CURLM *multi;
  int counter;
  kuul_t *active;
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

typedef struct kuul_task_s {
  TASK_TYPE type;
  kuul_t *k;
  char *data;
  int data_len;
} kuul_task_t;

typedef struct kuul_buf_s {
  char *buf;
  int len;
  int max;
} kuul_buf_t;

typedef struct kuul_data_s {
  char *start;
  char *rp;
  char *ep;
  kuul_t *k;
  struct kuul_data_s *next;
} kuul_data_t;

struct kuul_s {
  kuul_global *g;
  kuul_t *next;
  CURL *easy;
  kuul_buf_t *querybuf;
  kuul_buf_t *formbuf;
  kuul_buf_t *buf;
  int chunked;
  int body_length;
  int bytes_written;
  kuul_data_t *body;
  kuul_data_t *tail;
  char curl_error_buf[CURL_ERROR_SIZE];
  
  void *opaque;
  kuul_complete_cb on_complete;
  kuul_headers_cb on_headers;
  kuul_body_chunk_cb on_body_chunk;
  
  KUUL_PROTOCOL protocol;
  KUUL_METHOD method;
  char *host;
  char *path;
  int port;
  struct curl_slist *headers;
  int closed;
  int paused;
  int in_progress;
};

typedef struct kuul_curl_socket_context_s {
  kuul_global *g;
  union { 
    uv_handle_t poll_h; 
    uv_poll_t poll;
  };
  int fd;
} kuul_curl_socket_context;

static kuul_buf_t *init_buf( void ) {
  kuul_buf_t *b = calloc(sizeof(*b), 1);
  return b;
}

static void buf_try_grow( kuul_buf_t *buf, int len ) {
  if(!buf->max) {
    buf->max = 8;
  }
  if(buf->max - buf->len <= len) {
    while(buf->max - buf->len <= len) {
      buf->max *= 2;
    }
    buf->buf = realloc(buf->buf, buf->max);
  }
}

static void buf_appendf( kuul_buf_t *buf, char const *fmt, ... ) {
  /*  FIXME try to write to buffer if possible instead of calculating length needed first.  */
  va_list args;
  va_list args2;
  va_start(args, fmt);
  va_copy(args2, args);
  int len = vsnprintf(NULL, 0, fmt, args);
  va_end(args);
  buf_try_grow(buf, len);
  vsprintf(buf->buf+buf->len, fmt, args2);
  va_end(args2);
  buf->len += len;
  buf->buf[buf->len] = 0;
}

static void uv_free_data( uv_handle_t *h ) {
  free(h->data);
}

static void add_data_chunk( kuul_t *k, char const *data, size_t len, int copy ) {
  if(len && data) {
    kuul_data_t *d = calloc(sizeof(*d), 1);
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

static kuul_t *remove_kuul_from_active( kuul_global *g, CURL *easy ) {
  kuul_t *res = g->active;
  if(g->active->easy == easy) {
    g->active = g->active->next;
  }
  else {
    kuul_t *tmp;
    for(tmp = g->active; tmp->next->easy != easy; tmp = tmp->next);
    res = tmp->next;
    tmp->next = tmp->next->next;
  }
  return res;
}

static void clean_up_finished( kuul_global *g ) {
  /*  see if any easy handles are complete and wrap them up if so  */
  CURLMsg* message = NULL;
  CURL* easy = NULL;
  int pending = 0;
  while((message = curl_multi_info_read(g->multi, &pending ))) {
    if(message->msg == CURLMSG_DONE) {
      easy = message->easy_handle;
      kuul_t *k = remove_kuul_from_active(g, easy);
      if(k->on_complete) {
	k->on_complete(k, k->opaque);
      }
      curl_multi_remove_handle(g->multi, easy);
      long status;
      curl_easy_getinfo(k->easy, CURLINFO_RESPONSE_CODE, &status);
      fprintf(stderr, "Curl status: %ld\n", status);
      fprintf(stderr, "Curl Err: %s\n", k->curl_error_buf);
      curl_easy_cleanup(k->easy);
      free(k);
    }
  }
}

static void loop_uv_injection( uv_poll_t *p, int status, int events ) {
  /*  somebody outside wants something done!  */
  kuul_global *g = p->data;
  kuul_task_t *tasks[32];
  int num_read = read(g->output, tasks, sizeof(kuul_task_t *)*32);
  if(num_read) {
    num_read /= sizeof(kuul_task_t *);
    int i;
    for(i=0; i<num_read; i++) {
      kuul_task_t *t = tasks[i];
      switch(t->type) {
      case TASK_START:
	curl_multi_add_handle(g->multi, t->k->easy);
	break;
      case TASK_WRITE:
	add_data_chunk(t->k, t->data, t->data_len, 0);
	if(t->k->paused) {
	  //fprintf(stderr, "unpausing easy\n");
	  t->k->paused = 0;
	  curl_easy_pause(t->k->easy, CURLPAUSE_CONT);
	  curl_multi_socket_all(g->multi, &g->counter);
	  clean_up_finished(g);
	}
	break;
      case TASK_END:
	t->k->closed = 1;
	if(t->k->paused) {
	  //fprintf(stderr, "unpausing easy\n");
	  t->k->paused = 0;
	  curl_easy_pause(t->k->easy, CURLPAUSE_CONT);
	  curl_multi_socket_all(g->multi, &g->counter);
	  //curl_multi_socket_action(g->multi, CURL_SOCKET_TIMEOUT, 0, &g->counter);
	  clean_up_finished(g);
	}
	break;
      case TASK_SHUTDOWN:
	break;
      }
    }
  }
}

static kuul_curl_socket_context* init_curl_socket_context( kuul_global * g, curl_socket_t s, CURL* easy) {
  kuul_curl_socket_context * c = calloc(sizeof(*c), 1);
  c->g = g;
  c->fd = s;
  uv_poll_init(&g->loop, &c->poll, c->fd);
  c->poll.data = c;
  curl_multi_assign(g->multi, s, c);
  return c;
}

static void destroy_curl_socket_context( kuul_curl_socket_context *c ) {
  uv_close(&c->poll_h, uv_free_data);
}

static void loop_uv_socket_action( uv_poll_t* p, int status, int events ) {
  /*  libuv says we have some sweet socket action; relay to libcurl  */
  kuul_curl_socket_context *c = p->data;
  kuul_global* g = c->g;
  int flags = ((events & UV_READABLE) ? CURL_CSELECT_IN : 0)|((events & UV_WRITABLE) ? CURL_CSELECT_OUT : 0);
  while(curl_multi_socket_action(g->multi, c->fd, flags, &g->counter) == CURLM_CALL_MULTI_PERFORM);
  //if(g->counter <= 0 && uv_is_active( &g->curl_timer_h)) {
  //fprintf(stderr, "Stopping timer\n");
  //uv_timer_stop(&g->curl_timer);
  //}
  clean_up_finished(g);
}

static int loop_curl_socket( CURL *easy, curl_socket_t s, int action, void *opaque, void *socket_opaque ) {
  /*  libcurl wants to register/deregister events on socket s; set it up with libuv  */
  kuul_global* g = opaque;
  kuul_curl_socket_context* context = NULL;
  if (!(action & CURL_POLL_REMOVE)) {
    context = socket_opaque ? socket_opaque : init_curl_socket_context(g, s, easy);
    int flags = ((action & CURL_POLL_IN) ? UV_READABLE : 0)|((action & CURL_POLL_OUT) ? UV_WRITABLE : 0);
    uv_poll_start(&context->poll, flags, loop_uv_socket_action);
  }
  else if((context = socket_opaque)) {
    uv_poll_stop( &context->poll );
    destroy_curl_socket_context(context);
  }
  return 0;
}

static void loop_uv_timeout( uv_timer_t *t ) {
  /*  libuv says we have a timeout; relay to libcurl  */
  //fprintf(stderr, "Timeout triggered\n");
  kuul_global * g = t->data;
  while (curl_multi_socket_action(g->multi, CURL_SOCKET_TIMEOUT, 0, &g->counter) == CURLM_CALL_MULTI_PERFORM);
  clean_up_finished(g);
}

static int loop_curl_set_timer(CURLM* multi, long timeout, void *opaque) {
  /*  libcurl wants to set a timeout; set it up with libuv  */
  //fprintf(stderr, "set_timer called with %ld\n", timeout);
  kuul_global *g = opaque;
  if(timeout == 0) {
    curl_multi_socket_action(g->multi, CURL_SOCKET_TIMEOUT, 0, &g->counter);
    clean_up_finished(g);
  }
  else if(timeout == -1) {
    if(uv_is_active(&g->curl_timer_h)) {
      //fprintf(stderr, "Stopping timer\n");
      uv_timer_stop(&g->curl_timer);
    }
  }
  else {
    //fprintf(stderr, "Starting timer\n");
    uv_timer_start(&g->curl_timer, loop_uv_timeout, timeout, 0 );
  }
  return 0;
}

static size_t loop_curl_write_body( char *buf, size_t size, size_t num, void *opaque ) {
  size_t res = size*num;
  for(size_t i=0; i<res; i++) {
    putchar(buf[i]);
  }
  return res;
}

static size_t loop_curl_read_body( char *buf, size_t size, size_t num, void *opaque ) {
  kuul_t *k = (kuul_t *)opaque;
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
      kuul_data_t *tmp = k->body;
      k->body = k->body->next;
      free(tmp->start);
      free(tmp);
    }
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
    //fprintf(stderr, "pausing easy\n");
    return CURL_READFUNC_PAUSE;
  }
  /*  chunk-encoded body  */
  if(k->closed) {
    return 0;
  }
  k->paused = 1;
  //fprintf(stderr, "pausing easy\n");
  return CURL_READFUNC_PAUSE;
}

static void *uv_thread( void *arg ) {
  /*  spin up a thread and run a loop until it exits  */
  kuul_global *g = arg;
  uv_run(&g->loop, UV_RUN_DEFAULT);
  while(uv_loop_close(&g->loop) == UV_EBUSY);
  return NULL;
}

kuul_global *kuul_init_global( void ) {
  curl_global_init(CURL_GLOBAL_ALL);
  kuul_global *g = calloc(sizeof(*g), 1);
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
  uv_poll_start(&g->inject, UV_READABLE, loop_uv_injection);
  
  g->multi = curl_multi_init();
  curl_multi_setopt(g->multi, CURLMOPT_SOCKETFUNCTION, loop_curl_socket);
  curl_multi_setopt(g->multi, CURLMOPT_SOCKETDATA, g);
  curl_multi_setopt(g->multi, CURLMOPT_TIMERFUNCTION, loop_curl_set_timer);
  curl_multi_setopt(g->multi, CURLMOPT_TIMERDATA, g);
  
  pthread_create(&g->thread, NULL, uv_thread, g);
  return g;
}

void kuul_cleanup( kuul_global *g ) {
  
}

void kuul_cleanup_async( kuul_global *g ) {
  
}

static kuul_task_t *init_task( kuul_t *k, TASK_TYPE type ) {
  kuul_task_t *t = calloc(sizeof(*t), 1);
  t->type = type;
  t->k = k;
  return t;
}

static void add_task( kuul_task_t *t ) {
  write(t->k->g->input, &t, sizeof(t));
}

static void start_request( kuul_t *k ) {
  if(!k->in_progress) {
    k->in_progress = 1;
    k->easy = curl_easy_init();
    if(k->port < 0) {
      k->port = (k->protocol == KUUL_HTTP) ? 80 : 443;
    }
    if(!k->path) {
      k->path = strdup("/");
    }
    buf_appendf(k->buf, "%s://%s:%d%s", 
		(k->protocol == KUUL_HTTP) ? "http" : "https", k->host, k->port, k->path);
    if(k->querybuf) {
      buf_appendf(k->buf, "?%s", k->querybuf->buf);
    }
    curl_easy_setopt(k->easy, CURLOPT_VERBOSE, 1L);
    curl_easy_setopt(k->easy, CURLOPT_MAX_SEND_SPEED_LARGE, 32000);
    curl_easy_setopt(k->easy, CURLOPT_URL, k->buf->buf);
    curl_easy_setopt(k->easy, CURLOPT_ERRORBUFFER, k->curl_error_buf);
    curl_easy_setopt(k->easy, CURLOPT_WRITEFUNCTION, loop_curl_write_body);
    curl_easy_setopt(k->easy, CURLOPT_WRITEDATA, NULL );
    switch(k->method) {
    case KUUL_GET:
      curl_easy_setopt(k->easy, CURLOPT_HTTPGET, 1L);
      break;
    case KUUL_PUT:
      curl_easy_setopt(k->easy, CURLOPT_UPLOAD, 1L);
      break;
    case KUUL_POST:
      curl_easy_setopt(k->easy, CURLOPT_POST, 1L);
      break;
    case KUUL_DELETE:
      curl_easy_setopt(k->easy, CURLOPT_CUSTOMREQUEST, "DELETE");
      break;
    }
    if(k->chunked || k->body) {
      curl_easy_setopt(k->easy, CURLOPT_READFUNCTION, loop_curl_read_body);
      curl_easy_setopt(k->easy, CURLOPT_READDATA, k);
      if(k->chunked) {
	kuul_header(k, "Transfer-Encoding", "chunked");
      }
      else {
	char buf[64];
	snprintf(buf, 64, "%d", k->body_length);
	kuul_header(k, "Content-Length", buf);
      }
    }
    if(k->headers) {
      curl_easy_setopt(k->easy, CURLOPT_HTTPHEADER, k->headers );
    }
    add_task(init_task(k, TASK_START));
  }
}

kuul_t *kuul_init( kuul_global *g, KUUL_METHOD m ) {
  kuul_t *k = calloc(sizeof(*k), 1);
  k->method = m;
  k->port = -1;
  k->body_length = -1;
  k->g = g;
  k->buf = init_buf();
  k->protocol = KUUL_HTTP;
  k->next = g->active;
  g->active = k;
  return k;
}

void kuul_protocol( kuul_t *k, KUUL_PROTOCOL p ) {
  k->protocol = p;
}

void kuul_host( kuul_t *k, char const *host ) {
  k->host = strdup(host);
}

void kuul_port( kuul_t *k, int port ) {
  k->port = port;
}

void kuul_path( kuul_t *k, char const *path ) {
  k->path = strdup(path);
}

void kuul_append( kuul_t *k, char const *key, char const *value ) {
  switch(k->method) {
  case KUUL_GET:
  case KUUL_DELETE:
    return kuul_append_query(k, key, value);
  default:
    return kuul_append_form(k, key, value);
  }
}

static void append_param( kuul_t *k, kuul_buf_t *buf, char const *key, char const *value ) {
  key = curl_easy_escape(k->easy, key, 0);
  value = curl_easy_escape(k->easy, value, 0);
  if(buf->len) {
    buf_appendf(buf, "&%s=%s", key, value);
  }
  else {
    buf_appendf(buf, "%s=%s", key, value);
  }
}

void kuul_append_query( kuul_t *k, char const *key, char const *value ) {
  if(key && key[0] && value) {
    if(!k->querybuf) {
      k->querybuf = init_buf();
    }
    append_param(k, k->querybuf, key, value);
  }
}

void kuul_append_form( kuul_t *k, char const *key, char const *value ) {
  if(key && key[0] && value) {
    if(!k->formbuf) {
      k->formbuf = init_buf();
    }
    append_param(k, k->formbuf, key, value);
  }
}

void kuul_header( kuul_t *k, char const *key, char const *value ) {
  if(key && key[0] && value) {
    char buf[1024];
    snprintf(buf, 1024, "%s: %s", key, value);
    k->headers = curl_slist_append(k->headers, buf);
  }
}

void kuul_write( kuul_t *k, char const *data, size_t len ) {
  if(!k->in_progress) {
    k->chunked = 1;
    add_data_chunk(k, data, len, 1);
    start_request(k);
  }
  else {
    kuul_task_t *t = init_task(k, TASK_WRITE);
    t->data = malloc(len);
    t->data_len = len;
    memcpy(t->data, data, len);
    add_task(t);
  }
}

void kuul_write_end( kuul_t *k, char const *data, size_t len ) {
  if(!k->in_progress) {
    k->body_length = len;
    add_data_chunk(k, data, len, 1);
    start_request(k);
  }
  else {
    kuul_write(k, data, len);
    kuul_end(k);
  }
}

void kuul_end( kuul_t *k ) {
  if(!k->in_progress) {
    if(k->formbuf) {
      k->body_length = k->formbuf->len;
      add_data_chunk(k, k->formbuf->buf, k->body_length, 1);
    }
    start_request(k);
  }
  add_task(init_task(k, TASK_END));
}
