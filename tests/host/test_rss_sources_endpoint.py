#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise RSS through Zephyr's real HTTP/1 dispatch and JSON decoder.

Only transport/session/storage adapters are faked. No radio, NVS or SD is touched.
"""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
ZEPHYR = Path(os.environ.get("ZEPHYR_BASE", ROOT.parent / "zephyr"))
SOURCE = (ROOT / "src/papermono_content_server.c").read_text()
HTTP1_SOURCE = (ZEPHYR / "subsys/net/lib/http/http_server_http1.c").read_text()
HTTP_CORE_SOURCE = (ZEPHYR / "subsys/net/lib/http/http_server_core.c").read_text()


def function(name, source=SOURCE):
    """Find a C function while ignoring braces in strings/chars/comments."""
    match = re.search(r"^(?:static )?(?:int|void|bool) " + name + r"\([^;]*?\)\n\{", source, re.M)
    if not match:
        raise AssertionError(f"missing C function: {name}")
    begin = source.index("{", match.start())
    state, escaped, depth, index = "code", False, 0, begin
    while index < len(source):
        ch = source[index]
        pair = source[index:index + 2]
        if state == "line":
            if ch == "\n": state = "code"
        elif state == "comment":
            if pair == "*/": state = "code"; index += 1
        elif state in ("string", "char"):
            if escaped: escaped = False
            elif ch == "\\": escaped = True
            elif ch == ('"' if state == "string" else "'"): state = "code"
        elif pair == "//": state = "line"; index += 1
        elif pair == "/*": state = "comment"; index += 1
        elif ch == '"': state = "string"
        elif ch == "'": state = "char"
        elif ch == "{": depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0: return source[match.start():index + 1] + "\n"
        index += 1
    raise AssertionError(f"unclosed C function: {name}")


PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/data/json.h>
#include <papermono/rss.h>
#define PAPERMONO_SERVER_BSS
#define CONFIG_HTTP_SERVER_MAX_CLIENTS 2
#define STREAMING_REQUEST_DEADLINE_MS 30000
enum http_status { HTTP_200_OK=200, HTTP_400_BAD_REQUEST=400,
 HTTP_401_UNAUTHORIZED=401, HTTP_408_REQUEST_TIMEOUT=408, HTTP_409_CONFLICT=409,
 HTTP_413_PAYLOAD_TOO_LARGE=413, HTTP_503_SERVICE_UNAVAILABLE=503 };
enum http_transaction_status { HTTP_SERVER_REQUEST_DATA_MORE,
 HTTP_SERVER_REQUEST_DATA_FINAL, HTTP_SERVER_TRANSACTION_ABORTED,
 HTTP_SERVER_TRANSACTION_COMPLETE };
enum { HTTP_GET, HTTP_PUT, HTTP_HEAD, HTTP_DELETE, HTTP_OPTIONS, HTTP_POST, HTTP_PATCH };
enum { HTTP_HEADER_STATUS_NONE, HTTP_HEADER_STATUS_OK };
enum { CONTENT_SERVER_BUSY_RSS_REQUEST };
enum { HTTP_RESOURCE_TYPE_DYNAMIC, HTTP1_BODY_STATE, HTTP1_MESSAGE_COMPLETE_STATE };
struct http_header_capture_ctx { int status; bool authorized; };
struct http_client_ctx {
 int method;
 int fd;
 bool http1_headers_sent;
 char *cursor;
 size_t data_len;
 int parser_state;
 char url_buffer[1];
 struct http_header_capture_ctx header_capture_ctx;
};
struct http_request_ctx { const uint8_t *data; size_t data_len;
 int headers_status; bool authorized; };
struct http_response_ctx { enum http_status status; const uint8_t *body;
 size_t body_len; bool final_chunk; size_t header_count; };
struct http_resource_detail {
 int type; uint32_t bitmask_of_supported_http_methods;
 const char *content_type; size_t path_len;
};
struct http_resource_detail_dynamic {
 struct http_resource_detail common;
 int (*cb)(struct http_client_ctx *, enum http_transaction_status,
  const struct http_request_ctx *, struct http_response_ctx *, void *);
 struct http_client_ctx *holder;
 void *user_data;
};
static struct http_response_ctx response;
static char response_buffer[12288];
static struct papermono_rss_source saved[PAPERMONO_RSS_SOURCE_MAX];
static size_t saved_count;
static int replace_calls, read_calls, authorization_calls, replace_result;
static uint32_t generation;
static int64_t now;
static bool busy, change_during_read;
static int64_t k_uptime_get(void) { return now; }
static void content_server_set_busy(int bit, bool value) { (void)bit; busy=value; }
static bool session_authorized(const struct http_request_ctx *request) {
 ++authorization_calls; return request->authorized;
}
size_t papermono_rss_source_count(void) { ++read_calls; return saved_count; }
uint32_t papermono_rss_sources_generation(void) { ++read_calls; return generation; }
int papermono_rss_source_get(size_t index, struct papermono_rss_source *out) {
 ++read_calls; if(index>=saved_count) return -ENOENT;
 *out=saved[index]; if(change_during_read) ++generation; return 0;
}
int papermono_rss_sources_replace(const struct papermono_rss_source *sources, size_t count) {
 ++replace_calls;
 if(replace_result) return replace_result;
 if(count>PAPERMONO_RSS_SOURCE_MAX) return -EINVAL;
 /* Public source storage's validation boundary is modeled here; its real
  * persistence and duplicate/URL rules have their own data-layer suite. */
 for(size_t i=0;i<count;++i) {
  if(!sources[i].id[0] || !sources[i].name[0] || strncmp(sources[i].url,"http://",7)) return -EINVAL;
  for(const unsigned char *p=(const unsigned char *)sources[i].name;*p;++p) {
   if(*p<0x20U || *p==0x7fU) return -EINVAL;
  }
 }
 memset(saved,0,sizeof(saved)); memcpy(saved,sources,count*sizeof(*sources));
 saved_count=count; ++generation; return 0;
}
#define K_FOREVER 0
#define K_NO_WAIT 1
#define K_MSEC(milliseconds) (milliseconds)
#define ZSOCK_SHUT_RD 0
struct k_mutex { int held; };
struct k_work { void (*handler)(struct k_work *); };
struct k_work_delayable { struct k_work work; bool pending; int64_t due; };
#define K_MUTEX_DEFINE(name) struct k_mutex name
#define K_WORK_DELAYABLE_DEFINE(name, callback) \
 struct k_work_delayable name={.work={.handler=callback}}
static int shutdown_calls, shutdown_fd, lock_busy_calls, lock_depth;
static int k_mutex_lock(struct k_mutex *lock, int timeout) {
 if(lock->held) { assert(timeout==K_NO_WAIT); ++lock_busy_calls; return -EBUSY; }
 lock->held=1; ++lock_depth; return 0;
}
static void k_mutex_unlock(struct k_mutex *lock) { assert(lock->held && lock_depth==1); lock->held=0; --lock_depth; }
static int k_work_cancel_delayable(struct k_work_delayable *work) { work->pending=false; return 0; }
static int k_work_reschedule(struct k_work_delayable *work, int64_t delay) {
 work->pending=true; work->due=now+delay; return 0;
}
static int zsock_shutdown(int fd, int how) {
 (void)how; assert(fd>=0 && lock_depth==1); ++shutdown_calls; shutdown_fd=fd; return 0;
}
'''

HTTP_ADAPTERS = r'''
#define SEND_RESPONSE(...) (0)
#define LOG_DBG(...) ((void)0)
static const char final_chunk[]="0\r\n\r\n";
static int framework_conflicts, response_probes;
static struct http_client_ctx *probe_client;
static int handle_http1_dynamic_resource(struct http_resource_detail_dynamic *detail,
 struct http_client_ctx *client);
static void client_release_resources(struct http_client_ctx *client);
static void http_server_release_client(struct http_client_ctx *client) {
 client_release_resources(client);
 client->fd=-1;
}
static int zsock_close(int fd) {
 assert(rss_request.owner==NULL || rss_request.fd!=fd); return 0;
}
static void populate_request_ctx(struct http_request_ctx *request, const char *data,
 size_t length, struct http_header_capture_ctx *headers) {
 *request=(struct http_request_ctx){(const uint8_t *)data,length,
  headers?headers->status:HTTP_HEADER_STATUS_NONE,headers&&headers->authorized};
}
static bool is_client_http10(struct http_client_ctx *client) { (void)client; return false; }
static int http_server_sendall(struct http_client_ctx *client, const char *data, size_t length) {
 (void)client; (void)data; (void)length; return 0;
}
static int send_http1_405(struct http_client_ctx *client) {
 (void)client; response.status=HTTP_400_BAD_REQUEST; return 0;
}
static int send_http1_409(struct http_client_ctx *client) {
 (void)client; ++framework_conflicts; response.status=HTTP_409_CONFLICT; return 0;
}
static int enter_http_done_state(struct http_client_ctx *client) { (void)client; return 0; }
struct test_resource { struct http_resource_detail *detail; };
static struct test_resource test_resource={&rss_sources_resource_detail.common};
#define HTTP_SERVICE_FOREACH(service) for(int service=0;service<1;++service)
#define HTTP_SERVICE_FOREACH_RESOURCE(service, resource) \
 for(struct test_resource *resource=&test_resource;resource;resource=NULL)
static int http1_dynamic_response(struct http_client_ctx *client,
 struct http_response_ctx *provided, struct http_resource_detail_dynamic *detail) {
 /* FINAL and GET must retain the framework holder while their shared body
  * is being sent, even though the application owner has already cleared. */
 assert(detail->holder==client);
 response=*provided;
 client->http1_headers_sent=true;
 if(probe_client) {
  struct http_client_ctx *other=probe_client; probe_client=NULL;
  const struct http_response_ctx held=*provided;
  static char expected[sizeof(response_buffer)];
  assert(provided->body_len<sizeof(expected));
  memcpy(expected,provided->body,provided->body_len);
  const int conflicts=framework_conflicts, reads=read_calls, writes=replace_calls;
  assert(handle_http1_dynamic_resource(detail,other)==0);
  assert(framework_conflicts==conflicts+1 && detail->holder==client);
  assert(read_calls==reads && replace_calls==writes);
  assert(!memcmp(expected,held.body,held.body_len));
  response=held; ++response_probes;
 }
 return 0;
}
'''


MAIN = r'''
static struct http_client_ctx a={.method=HTTP_PUT,.fd=10}, b={.method=HTTP_GET,.fd=11};
static const char valid[]="{\"sources\":[{\"id\":\"gold\",\"name\":\"金价\",\"url\":\"http://example.test/private-token/gold.xml\"}]}";
static int last_dispatch;
static void clean(void) {
 memset(saved,0,sizeof(saved)); saved_count=1;
 snprintf(saved[0].id,sizeof(saved[0].id),"old");
 snprintf(saved[0].name,sizeof(saved[0].name),"旧订阅");
 snprintf(saved[0].url,sizeof(saved[0].url),"http://example.test/private-token/old.xml");
 memset(&rss_request,0,sizeof(rss_request));
 memset(&response,0,sizeof(response)); memset(response_buffer,0,sizeof(response_buffer));
 memset(&a,0,sizeof(a)); memset(&b,0,sizeof(b));
 replace_calls=read_calls=authorization_calls=replace_result=0;
 generation=1; now=0; busy=change_during_read=false;
 a.method=HTTP_PUT; a.fd=10; b.method=HTTP_GET; b.fd=11;
 rss_sources_resource_detail.holder=NULL;
 rss_request_timeout.pending=false; rss_request_timeout.due=0;
 shutdown_calls=0; shutdown_fd=-1; last_dispatch=0;
 lock_busy_calls=0; assert(!lock_depth);
 framework_conflicts=response_probes=0; probe_client=NULL;
 assert(!rss_request_lock.held);
}
static void close_transport(struct http_client_ctx *client) {
 /* Real core release sends ABORT while the captured fd is still valid;
  * only after that callback may the transport close/reuse its descriptor. */
 const int fd=client->fd;
 assert(fd>=0); close_client_connection(client); assert(client->fd<0);
}
static void deliver(struct http_client_ctx *client, enum http_transaction_status status,
 const char *data, size_t length, bool headers, bool authorized) {
 memset(&response,0,sizeof(response)); last_dispatch=0;
 if(headers && client->fd<0) client->fd=client==&a?10:11;
 if(client->fd<0) { last_dispatch=-ENOTCONN; return; }
 if(status==HTTP_SERVER_TRANSACTION_ABORTED) { close_transport(client); return; }
 if(status==HTTP_SERVER_TRANSACTION_COMPLETE) {
  struct http_request_ctx request={NULL,0,HTTP_HEADER_STATUS_NONE,false};
  assert(rss_sources_handler(client,status,&request,&response,NULL)==0); return;
 }
 if(headers) client->http1_headers_sent=false;
 client->cursor=(char *)data; client->data_len=length;
 client->parser_state=status==HTTP_SERVER_REQUEST_DATA_FINAL?
  HTTP1_MESSAGE_COMPLETE_STATE:HTTP1_BODY_STATE;
 client->header_capture_ctx=(struct http_header_capture_ctx){
  headers?HTTP_HEADER_STATUS_OK:HTTP_HEADER_STATUS_NONE,authorized};
 last_dispatch=handle_http1_dynamic_resource(&rss_sources_resource_detail,client);
 /* This is the real server's negative-handler-result path: core resource
  * cleanup and ABORT run before the socket/slot can be recycled. */
 if(last_dispatch<0) close_transport(client);
}
static void final(struct http_client_ctx *client, const char *data, bool authorized) {
 deliver(client,HTTP_SERVER_REQUEST_DATA_FINAL,data,strlen(data),true,authorized);
}
static void cleared(void) {
 static const unsigned char zero[sizeof(rss_request)];
 assert(!busy && memcmp(&rss_request,zero,sizeof(rss_request))==0);
 assert(!rss_request_timeout.pending && !rss_request_lock.held);
 assert(rss_sources_resource_detail.holder==NULL);
}
static void fire_deadline(int64_t timestamp) {
 now=timestamp;
 if(rss_request_timeout.pending && now>=rss_request_timeout.due) {
  rss_request_timeout.pending=false;
  rss_request_timeout.work.handler(&rss_request_timeout.work);
 }
}
static void rejected(const char *json) {
 clean(); final(&a,json,true);
 if(response.status!=HTTP_400_BAD_REQUEST) fprintf(stderr,"expected 400, got %d: %s\n",response.status,json);
 assert(response.status==HTTP_400_BAD_REQUEST);
 assert(saved_count==1 && strcmp(saved[0].id,"old")==0); cleared();
}
static void escaped_name_boundary(char special) {
 char escaped[PAPERMONO_RSS_SOURCE_NAME_MAX*2U+1U], json[512];
 for(size_t i=0;i<PAPERMONO_RSS_SOURCE_NAME_MAX;++i) {
  escaped[i*2U]='\\'; escaped[i*2U+1U]=special;
 }
 escaped[sizeof(escaped)-1U]='\0';
 snprintf(json,sizeof(json),"{\"sources\":[{\"id\":\"g\",\"name\":\"%s\",\"url\":\"http://example.test/x\"}]}",escaped);
 clean(); final(&a,json,true);
 assert(response.status==HTTP_200_OK && replace_calls==1);
 assert(strlen(saved[0].name)==PAPERMONO_RSS_SOURCE_NAME_MAX);
 for(size_t i=0;i<PAPERMONO_RSS_SOURCE_NAME_MAX;++i) assert(saved[0].name[i]==special);
 cleared();
}
static void decoded_field_boundaries(void) {
 char id[PAPERMONO_RSS_SOURCE_ID_MAX+2U], name[PAPERMONO_RSS_SOURCE_NAME_MAX+2U];
 char url[PAPERMONO_RSS_SOURCE_URL_MAX+2U], escaped_url[PAPERMONO_RSS_SOURCE_URL_MAX*2U+1U], json[768];
 memset(id,'i',sizeof(id)); id[PAPERMONO_RSS_SOURCE_ID_MAX]='\0';
 memset(name,'n',sizeof(name)); name[PAPERMONO_RSS_SOURCE_NAME_MAX]='\0';
 memset(url,'x',sizeof(url)); memcpy(url,"http://example.test/",sizeof("http://example.test/")-1U);
 url[PAPERMONO_RSS_SOURCE_URL_MAX]='\0';
 clean(); snprintf(json,sizeof(json),"{\"sources\":[{\"id\":\"%s\",\"name\":\"%s\",\"url\":\"%s\"}]}",id,name,url);
 final(&a,json,true); assert(response.status==HTTP_200_OK && replace_calls==1); cleared();
 /* Escaped slashes also count by their decoded length, at the URL limit. */
 size_t escaped=0U;
 for(size_t i=0U;i<PAPERMONO_RSS_SOURCE_URL_MAX;++i) {
  if(url[i]=='/') escaped_url[escaped++]='\\';
  escaped_url[escaped++]=url[i];
 }
 escaped_url[escaped]='\0';
 clean(); snprintf(json,sizeof(json),"{\"sources\":[{\"id\":\"g\",\"name\":\"n\",\"url\":\"%s\"}]}",escaped_url);
 final(&a,json,true); assert(response.status==HTTP_200_OK && !strcmp(saved[0].url,url)); cleared();
 id[PAPERMONO_RSS_SOURCE_ID_MAX]='i'; id[PAPERMONO_RSS_SOURCE_ID_MAX+1U]='\0';
 snprintf(json,sizeof(json),"{\"sources\":[{\"id\":\"%s\",\"name\":\"n\",\"url\":\"http://example.test/x\"}]}",id);
 rejected(json); assert(!replace_calls);
 name[PAPERMONO_RSS_SOURCE_NAME_MAX]='n'; name[PAPERMONO_RSS_SOURCE_NAME_MAX+1U]='\0';
 snprintf(json,sizeof(json),"{\"sources\":[{\"id\":\"g\",\"name\":\"%s\",\"url\":\"http://example.test/x\"}]}",name);
 rejected(json); assert(!replace_calls);
 url[PAPERMONO_RSS_SOURCE_URL_MAX]='x'; url[PAPERMONO_RSS_SOURCE_URL_MAX+1U]='\0';
 snprintf(json,sizeof(json),"{\"sources\":[{\"id\":\"g\",\"name\":\"n\",\"url\":\"%s\"}]}",url);
 rejected(json); assert(!replace_calls);
}
int main(void) {
 clean(); final(&b,"",true);
 assert(response.status==HTTP_200_OK && response.final_chunk);
 assert(strstr((const char *)response.body,"private-token/old.xml"));
 assert(strstr((const char *)response.body,"旧订阅")); cleared();
 clean(); final(&b,"",false);
 assert(response.status==HTTP_401_UNAUTHORIZED && !read_calls && !replace_calls); cleared();
 clean(); final(&a,valid,false);
 assert(response.status==HTTP_401_UNAUTHORIZED && !read_calls && !replace_calls); cleared();

 /* An unauthorized MORE closes through core ABORT immediately. It cannot
  * reserve this dynamic resource until a slow sender finishes its body. */
 clean(); deliver(&a,HTTP_SERVER_REQUEST_DATA_MORE,"{",1,true,false);
 assert(last_dispatch==-EACCES && a.fd<0); cleared();
 final(&b,"",true); assert(response.status==HTTP_200_OK && !framework_conflicts);
 deliver(&a,HTTP_SERVER_REQUEST_DATA_FINAL,valid,strlen(valid),false,false);
 assert(last_dispatch==-ENOTCONN && !replace_calls); cleared();
 final(&a,valid,false);
 assert(response.status==HTTP_401_UNAUTHORIZED && !replace_calls); cleared();

 /* GET and successful FINAL PUT retain the holder during transmission.
  * A competing dispatch cannot overwrite the shared response buffer. */
 clean(); a.method=HTTP_GET; probe_client=&a; final(&b,"",true);
 assert(response.status==HTTP_200_OK && response_probes==1 && framework_conflicts==1);
 assert(strstr((const char *)response.body,"private-token/old.xml")); cleared();
 clean(); probe_client=&b; final(&a,valid,true);
 assert(response.status==HTTP_200_OK && response_probes==1 && framework_conflicts==1);
 assert(saved_count==1 && !strcmp(saved[0].id,"gold")); cleared();

 /* Capture authorization once, preserve the real framework holder through
  * MORE, and prevent a second client's body or ABORT touching the owner. */
 clean(); size_t cut=strlen(valid)/2;
 deliver(&a,HTTP_SERVER_REQUEST_DATA_MORE,valid,cut,true,true);
 assert(busy && rss_request.owner==&a && authorization_calls==1);
 assert(rss_sources_resource_detail.holder==&a && rss_request_timeout.pending);
 final(&b,"bad",false);
 assert(response.status==HTTP_409_CONFLICT && framework_conflicts==1 && !replace_calls);
 assert(rss_request.length==cut && rss_request.owner==&a);
 deliver(&b,HTTP_SERVER_TRANSACTION_ABORTED,"",0,false,false);
 assert(busy && rss_request.owner==&a && rss_request.length==cut);
 deliver(&a,HTTP_SERVER_REQUEST_DATA_FINAL,valid+cut,strlen(valid)-cut,false,false);
 assert(response.status==HTTP_200_OK && authorization_calls==1 && replace_calls==1);
 assert(!strcmp(saved[0].name,"金价") && !strcmp(saved[0].id,"gold")); cleared();

 /* The real ABORT path clears secrets, deadline and busy before slot reuse. */
 clean(); deliver(&a,HTTP_SERVER_REQUEST_DATA_MORE,valid,cut,true,true);
 deliver(&a,HTTP_SERVER_TRANSACTION_ABORTED,"",0,false,false); cleared();
 final(&a,valid,false);
 assert(response.status==HTTP_401_UNAUTHORIZED && !replace_calls); cleared();
 final(&a,valid,true); assert(response.status==HTTP_200_OK && replace_calls==1); cleared();

 /* Proper unescaping is observable at the backend boundary. */
 clean(); final(&a,"{\"sources\":[{\"id\":\"g\",\"name\":\"金\\\"价\\\\表\",\"url\":\"http:\\/\\/example.test/x\"}]}",true);
 assert(response.status==HTTP_200_OK && !strcmp(saved[0].name,"金\"价\\表"));
 assert(!strcmp(saved[0].url,"http://example.test/x")); cleared();
 clean(); final(&a,"{\"sources\":[{\"id\":\"g\",\"name\":\"\\u91d1\\u4ef7\",\"url\":\"http://example.test/x\"}]}",true);
 assert(response.status==HTTP_200_OK && !strcmp(saved[0].name,"金价")); cleared();
 clean(); final(&a,"{\"sources\":[{\"id\":\"g\",\"name\":\"\\uD83D\\uDCC8\",\"url\":\"http://example.test/x\"}]}",true);
 assert(response.status==HTTP_200_OK && !strcmp(saved[0].name,"📈")); cleared();
 clean(); final(&a,"{\"sources\":[{\"id\":\"g\",\"name\":\"\\\\u91d1\",\"url\":\"http://example.test/x\"}]}",true);
 assert(response.status==HTTP_200_OK && !strcmp(saved[0].name,"\\u91d1")); cleared();
 escaped_name_boundary('"'); escaped_name_boundary('\\'); decoded_field_boundaries();

 /* Only assembled JSON is decoded, including split UTF-8 units. */
 clean(); cut=(size_t)(strstr(valid,"金")-valid)+1U;
 deliver(&a,HTTP_SERVER_REQUEST_DATA_MORE,valid,cut,true,true);
 deliver(&a,HTTP_SERVER_REQUEST_DATA_FINAL,valid+cut,strlen(valid)-cut,false,false);
 assert(response.status==HTTP_200_OK && !strcmp(saved[0].name,"金价")); cleared();
 clean(); final(&a,"{\"sources\":[{\"id\":\"g\",\"name\":\"长长长长长长长长长长长长长长长ab\",\"url\":\"http://example.test/x\"}]}",true);
 assert(response.status==HTTP_200_OK && strlen(saved[0].name)==PAPERMONO_RSS_SOURCE_NAME_MAX); cleared();
 rejected("{}"); rejected("{\"sources\":{}}");
 rejected("{\"sources\":[{\"id\":\"x\",\"url\":\"http://example.test/x\"}]}");
 rejected("{\"sources\":[{\"id\":\"x\",\"name\":\"x\"}]}");
 rejected("{\"sources\":[{}, {}, {}, {}, {}]}");
 rejected("{\"sources\":[{\"id\":\"g\",\"name\":\"长长长长长长长长长长长长长长长长\",\"url\":\"http://example.test/x\"}]}");
 rejected("{\"sources\":[{\"id\":\"g\",\"name\":\"\\u0000\",\"url\":\"http://example.test/x\"}]}");
 rejected("{\"sources\":[{\"id\":\"g\",\"name\":\"\\uD800\",\"url\":\"http://example.test/x\"}]}");
 rejected("{\"sources\":[{\"id\":\"g\",\"name\":\"\\uDC00\",\"url\":\"http://example.test/x\"}]}");
 rejected("{\"sources\":[{\"id\":\"g\",\"name\":\"bad\\nname\",\"url\":\"http://example.test/x\"}]}");
 clean(); final(&a,"{\"sources\":[]}",true);
 assert(response.status==HTTP_200_OK && saved_count==0 && replace_calls==1); cleared();
 clean(); replace_result=-ENOSPC; final(&a,valid,true);
 assert(response.status==HTTP_503_SERVICE_UNAVAILABLE && saved_count==1 && !strcmp(saved[0].id,"old")); cleared();
 clean(); change_during_read=true; final(&b,"",true);
 assert(response.status==HTTP_409_CONFLICT); cleared();

 /* Too-large MORE closes before completion and leaves no stale owner. */
 clean(); deliver(&a,HTTP_SERVER_REQUEST_DATA_MORE,"x",RSS_REQUEST_LIMIT+1U,true,true);
 assert(last_dispatch==-EFBIG && a.fd<0 && !replace_calls); cleared();
 b.method=HTTP_PUT; cut=strlen(valid)/2;
 deliver(&b,HTTP_SERVER_REQUEST_DATA_MORE,valid,cut,true,true);
 deliver(&a,HTTP_SERVER_REQUEST_DATA_FINAL,"garbage",7,false,false);
 assert(last_dispatch==-ENOTCONN && rss_request.owner==&b && rss_request.length==cut);
 deliver(&b,HTTP_SERVER_REQUEST_DATA_FINAL,valid+cut,strlen(valid)-cut,false,false);
 assert(response.status==HTTP_200_OK && replace_calls==1); cleared();
 clean(); deliver(&a,HTTP_SERVER_REQUEST_DATA_FINAL,"x",RSS_REQUEST_LIMIT+1U,true,true);
 assert(response.status==HTTP_413_PAYLOAD_TOO_LARGE && !replace_calls); cleared();

 /* A deadline fires without any further request, even when a slow sender
  * repeatedly refreshed Zephyr's separate 10-second inactivity timeout. */
 clean(); deliver(&a,HTTP_SERVER_REQUEST_DATA_MORE,"{",1,true,true);
 for(int64_t at=9000;at<=27000;at+=9000) {
  fire_deadline(at); deliver(&a,HTTP_SERVER_REQUEST_DATA_MORE," ",1,false,false);
  assert(!shutdown_calls && busy && rss_request.owner==&a);
 }
 fire_deadline(STREAMING_REQUEST_DEADLINE_MS);
 assert(shutdown_calls==1 && shutdown_fd==a.fd && busy);
 close_transport(&a); cleared();
 final(&b,"",true); assert(response.status==HTTP_200_OK && !framework_conflicts); cleared();
 b.method=HTTP_PUT; cut=strlen(valid)/2;
 deliver(&b,HTTP_SERVER_REQUEST_DATA_MORE,valid,cut,true,true);
 deliver(&a,HTTP_SERVER_REQUEST_DATA_MORE,"garbage",7,false,false);
 assert(last_dispatch==-ENOTCONN && rss_request.owner==&b && rss_request.length==cut);
 deliver(&b,HTTP_SERVER_REQUEST_DATA_FINAL,valid+cut,strlen(valid)-cut,false,false);
 assert(response.status==HTTP_200_OK && replace_calls==1); cleared();

 /* If work execution is delayed, the next callback enforces the same
  * deadline; FINAL can still send 408 normally before releasing its holder. */
 clean(); deliver(&a,HTTP_SERVER_REQUEST_DATA_MORE,"{",1,true,true);
 now=STREAMING_REQUEST_DEADLINE_MS;
 deliver(&a,HTTP_SERVER_REQUEST_DATA_MORE," ",1,false,false);
 assert(last_dispatch==-ETIMEDOUT && a.fd<0 && !replace_calls); cleared();
 clean(); deliver(&a,HTTP_SERVER_REQUEST_DATA_MORE,valid,cut,true,true);
 now=STREAMING_REQUEST_DEADLINE_MS;
 deliver(&a,HTTP_SERVER_REQUEST_DATA_FINAL,valid+cut,strlen(valid)-cut,false,false);
 assert(response.status==HTTP_408_REQUEST_TIMEOUT && !replace_calls); cleared();

 /* FINAL can be doing SD I/O under this lock. The system-queue deadline
  * must return immediately, touch no storage, and retry after 100 ms. */
 clean(); deliver(&a,HTTP_SERVER_REQUEST_DATA_MORE,"{",1,true,true);
 assert(k_mutex_lock(&rss_request_lock,K_FOREVER)==0);
 fire_deadline(STREAMING_REQUEST_DEADLINE_MS);
 assert(lock_busy_calls==1 && lock_depth==1 && !shutdown_calls);
 assert(rss_request_timeout.pending && rss_request_timeout.due==30100);
 assert(!replace_calls && !read_calls && rss_request.owner==&a);
 k_mutex_unlock(&rss_request_lock);
 fire_deadline(30100);
 assert(shutdown_calls==1 && shutdown_fd==a.fd);
 close_transport(&a); cleared();

 /* A previously canceled work invocation cannot shut down a reused fd or
  * a new owner early. It checks the current owner's deadline under the lock. */
 clean(); deliver(&a,HTTP_SERVER_REQUEST_DATA_MORE,"{",1,true,true);
 close_transport(&a); cleared();
 now=1000; b.method=HTTP_PUT;
 deliver(&b,HTTP_SERVER_REQUEST_DATA_MORE,valid,cut,true,true);
 now=STREAMING_REQUEST_DEADLINE_MS;
 rss_request_timeout.work.handler(&rss_request_timeout.work);
 assert(!shutdown_calls && rss_request.owner==&b && rss_request_timeout.due==31000);
 deliver(&b,HTTP_SERVER_REQUEST_DATA_FINAL,valid+cut,strlen(valid)-cut,false,false);
 assert(response.status==HTTP_200_OK); cleared();
 rss_request_timeout.work.handler(&rss_request_timeout.work);
 assert(!shutdown_calls); cleared();
 puts("RSS sources endpoint: real dispatch/ABORT, deadline shutdown, fd reuse, escaped bounds and JSON: PASS");
 return 0;
}
'''


SHIMS = {
    "zephyr/sys/util.h": r'''#pragma once
#include <stddef.h>
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define SIZEOF_FIELD(type, member) sizeof(((type *)0)->member)
#define ARG_UNUSED(x) (void)(x)
#define ROUND_UP(x, a) ((((x) + (a) - 1) / (a)) * (a))
#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)
#define BIT_MASK(n) ((1ULL << (n)) - 1ULL)
#define BIT(n) (1U << (n))
''',
    "zephyr/sys/__assert.h": "#include <assert.h>\n#define __ASSERT_NO_MSG(x) assert(x)\n",
    "zephyr/sys/printk.h": "#include <stdio.h>\n#define snprintk snprintf\n",
    "zephyr/toolchain.h": "#define __fallthrough __attribute__((fallthrough))\n",
    "zephyr/types.h": "#include <stdint.h>\n#include <stdbool.h>\n#include <inttypes.h>\n",
}


class RssEndpointTests(unittest.TestCase):
    def test_real_handler_and_json(self):
        wire_start = SOURCE.index("#define RSS_REQUEST_LIMIT")
        wire_end = SOURCE.index("#endif", wire_start)
        handler_start = SOURCE.rfind("#if defined(CONFIG_PAPERMONO_RSS)", 0, SOURCE.index("static void rss_request_clear"))
        handler_end = SOURCE.index("#endif", handler_start)
        resource_start = SOURCE.index("static struct http_resource_detail_dynamic rss_sources_resource_detail =")
        resource_end = SOURCE.index("\n};", resource_start) + len("\n};")
        http_functions = (
            function("http_response_is_final", HTTP_CORE_SOURCE)
            + function("http_response_is_provided", HTTP_CORE_SOURCE)
            + function("client_release_resources", HTTP_CORE_SOURCE)
            + function("close_client_connection", HTTP_CORE_SOURCE)
            + function("dynamic_get_del_opts_req", HTTP1_SOURCE)
            + function("dynamic_post_put_req", HTTP1_SOURCE)
            + function("handle_http1_dynamic_resource", HTTP1_SOURCE)
        )
        code = (PREFIX + SOURCE[wire_start:wire_end] + function("response_json")
                + function("append_json_string") + SOURCE[handler_start:handler_end].split("\n", 1)[1]
                + SOURCE[resource_start:resource_end] + HTTP_ADAPTERS + http_functions + MAIN)
        with tempfile.TemporaryDirectory(prefix="papermono-rss-endpoint-") as directory:
            root = Path(directory)
            for name, content in SHIMS.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            source = root / "endpoint.c"
            executable = root / "endpoint-test"
            source.write_text(code)
            subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
                "-std=c17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                "-I", str(root), "-I", str(ROOT / "include"),
                "-I", str(ZEPHYR / "include"), str(source),
                str(ZEPHYR / "lib/utils/json.c"), "-lm", "-o", str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
