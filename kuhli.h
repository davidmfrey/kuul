#ifndef KUHLI_H
#define KUHLI_H
#include <stddef.h>
#include <curl/curl.h>

#ifdef __cplusplus
extern "C" {
#endif
  
  typedef struct kuhli_global_t kuhli_global_t;
  typedef struct kuhli_t kuhli_t;
  
  typedef void (*kuhli_complete_cb)(kuhli_t *, int status, void *opaque);
  typedef void (*kuhli_headers_cb)(kuhli_t *, void *opaque);
  typedef void (*kuhli_body_chunk_cb)(kuhli_t *, char *chunk, size_t length, void *opaque);
  
  typedef enum { KUHLI_HTTP,
		 KUHLI_HTTPS } KUHLI_PROTOCOL;

  typedef enum { KUHLI_GET,
		 KUHLI_PUT,
		 KUHLI_POST,
		 KUHLI_DELETE } KUHLI_METHOD;
  
  kuhli_global_t *kuhli_global_init( void );
  void kuhli_global_cleanup( kuhli_global_t * );
  void kuhli_global_cleanup_async( kuhli_global_t * );
  
  kuhli_t *kuhli_init( kuhli_global_t *, KUHLI_METHOD );
  
  void kuhli_on_headers( kuhli_t *, kuhli_headers_cb );
  void kuhli_on_body_chunk( kuhli_t *, kuhli_body_chunk_cb );
  void kuhli_on_complete( kuhli_t *, kuhli_complete_cb );
  void kuhli_opaque( kuhli_t *, void *opaque );
  
  void kuhli_verbose( kuhli_t *, int );
  
  void kuhli_protocol( kuhli_t *, KUHLI_PROTOCOL );
  void kuhli_host( kuhli_t *, char const *host );
  void kuhli_port( kuhli_t *, int port );
  void kuhli_path( kuhli_t *, char const *path );
  void kuhli_append( kuhli_t *, char const *k, char const *v );
  void kuhli_append_query( kuhli_t *, char const *k, char const *v );
  void kuhli_append_form( kuhli_t *, char const *k, char const *v );
  void kuhli_header( kuhli_t *, char const *k, char const *v );
  int kuhli_write( kuhli_t *, char const *data, size_t len );
  int kuhli_write_end( kuhli_t *, char const *data, size_t len );
  void kuhli_end( kuhli_t * );

#ifdef __cplusplus
}
#endif

#endif
