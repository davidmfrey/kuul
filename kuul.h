#ifndef KUUL_H
#define KUUL_H
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kuul_global_s kuul_global;
typedef struct kuul_s kuul_t;

typedef void (*kuul_complete_cb)(kuul_t *, void *opaque);
typedef void (*kuul_headers_cb)(kuul_t *, void *opaque);
typedef void (*kuul_body_chunk_cb)(kuul_t *, char *chunk, size_t length, void *opaque);

typedef enum { KUUL_HTTP,
	       KUUL_HTTPS } KUUL_PROTOCOL;

typedef enum { KUUL_GET,
	       KUUL_PUT,
	       KUUL_POST,
	       KUUL_DELETE } KUUL_METHOD;

kuul_global *kuul_init_global( void );
void kuul_cleanup( kuul_global * );

kuul_t *kuul_init( kuul_global *, KUUL_METHOD );

void kuul_protocol( kuul_t *, KUUL_PROTOCOL );
void kuul_host( kuul_t *, char const *host );
void kuul_port( kuul_t *, int port );
void kuul_path( kuul_t *, char const *path );
void kuul_append( kuul_t *, char const *k, char const *v );
void kuul_append_query( kuul_t *, char const *k, char const *v );
void kuul_append_form( kuul_t *, char const *k, char const *v );
void kuul_header( kuul_t *, char const *k, char const *v );
void kuul_write( kuul_t *, char const *data, size_t len );
void kuul_write_end( kuul_t *, char const *data, size_t len );
void kuul_end( kuul_t * );

#ifdef __cplusplus
}
#endif

#endif
