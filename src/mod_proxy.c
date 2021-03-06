#include "first.h"

#include <string.h>
#include <stdlib.h>

#include "gw_backend.h"
#include "base.h"
#include "array.h"
#include "buffer.h"
#include "inet_ntop_cache.h"
#include "keyvalue.h"
#include "log.h"
#include "plugin.h"
#include "status_counter.h"

/**
 *
 * HTTP reverse proxy
 *
 * TODO:      - HTTP/1.1
 *            - HTTP/1.1 persistent connection with upstream servers
 */

/* (future: might split struct and move part to http-header-glue.c) */
typedef struct http_header_remap_opts {
    const array *urlpaths;
    const array *hosts_request;
    const array *hosts_response;
    int https_remap;
    int upgrade;
    /*(not used in plugin_config, but used in handler_ctx)*/
    const buffer *http_host;
    const buffer *forwarded_host;
    const data_string *forwarded_urlpath;
} http_header_remap_opts;

typedef enum {
	PROXY_FORWARDED_NONE         = 0x00,
	PROXY_FORWARDED_FOR          = 0x01,
	PROXY_FORWARDED_PROTO        = 0x02,
	PROXY_FORWARDED_HOST         = 0x04,
	PROXY_FORWARDED_BY           = 0x08,
	PROXY_FORWARDED_REMOTE_USER  = 0x10
} proxy_forwarded_t;

typedef struct {
	gw_plugin_config gw;
	array *forwarded_params;
	array *header_params;
	unsigned short replace_http_host;
	unsigned int forwarded;

	http_header_remap_opts header;
} plugin_config;

typedef struct {
	PLUGIN_DATA;
	plugin_config **config_storage;

	plugin_config conf;
} plugin_data;

static int proxy_check_extforward;

typedef struct {
	gw_handler_ctx gw;
	http_response_opts opts;
	http_header_remap_opts remap_hdrs;
	plugin_config conf;
} handler_ctx;


INIT_FUNC(mod_proxy_init) {
	plugin_data *p;

	p = calloc(1, sizeof(*p));

	return p;
}


FREE_FUNC(mod_proxy_free) {
	plugin_data *p = p_d;

	UNUSED(srv);

	if (p->config_storage) {
		size_t i;
		for (i = 0; i < srv->config_context->used; i++) {
			plugin_config *s = p->config_storage[i];

			if (NULL == s) continue;

			array_free(s->forwarded_params);
			array_free(s->header_params);

			/*assert(0 == offsetof(s->gw));*/
			gw_plugin_config_free(&s->gw);
			/*free(s);*//*free'd by gw_plugin_config_free()*/
		}
		free(p->config_storage);
	}

	free(p);

	return HANDLER_GO_ON;
}

SETDEFAULTS_FUNC(mod_proxy_set_defaults) {
	plugin_data *p = p_d;
	data_unset *du;
	size_t i = 0;

	config_values_t cv[] = {
		{ "proxy.server",              NULL, T_CONFIG_LOCAL, T_CONFIG_SCOPE_CONNECTION },       /* 0 */
		{ "proxy.debug",               NULL, T_CONFIG_SHORT, T_CONFIG_SCOPE_CONNECTION },       /* 1 */
		{ "proxy.balance",             NULL, T_CONFIG_LOCAL, T_CONFIG_SCOPE_CONNECTION },       /* 2 */
		{ "proxy.replace-http-host",   NULL, T_CONFIG_BOOLEAN, T_CONFIG_SCOPE_CONNECTION },     /* 3 */
		{ "proxy.forwarded",           NULL, T_CONFIG_ARRAY, T_CONFIG_SCOPE_CONNECTION },       /* 4 */
		{ "proxy.header",              NULL, T_CONFIG_ARRAY, T_CONFIG_SCOPE_CONNECTION },       /* 5 */
		{ "proxy.map-extensions",      NULL, T_CONFIG_ARRAY, T_CONFIG_SCOPE_CONNECTION },       /* 6 */
		{ NULL,                        NULL, T_CONFIG_UNSET, T_CONFIG_SCOPE_UNSET }
	};

	p->config_storage = calloc(1, srv->config_context->used * sizeof(plugin_config *));

	for (i = 0; i < srv->config_context->used; i++) {
		data_config const* config = (data_config const*)srv->config_context->data[i];
		plugin_config *s;

		s = calloc(1, sizeof(plugin_config));
		s->gw.debug          = 0;
		s->replace_http_host = 0;
		s->forwarded_params  = array_init();
		s->forwarded         = PROXY_FORWARDED_NONE;
		s->header_params     = array_init();
		s->gw.ext_mapping    = array_init();

		cv[0].destination = NULL; /* T_CONFIG_LOCAL */
		cv[1].destination = &(s->gw.debug);
		cv[2].destination = NULL; /* T_CONFIG_LOCAL */
		cv[3].destination = &(s->replace_http_host);
		cv[4].destination = s->forwarded_params;
		cv[5].destination = s->header_params;
		cv[6].destination = s->gw.ext_mapping;

		p->config_storage[i] = s;

		if (0 != config_insert_values_global(srv, config->value, cv, i == 0 ? T_CONFIG_SCOPE_SERVER : T_CONFIG_SCOPE_CONNECTION)) {
			return HANDLER_ERROR;
		}

		du = array_get_element(config->value, "proxy.server");
		if (!gw_set_defaults_backend(srv, (gw_plugin_data *)p, du, i, 0)) {
			return HANDLER_ERROR;
		}

		du = array_get_element(config->value, "proxy.balance");
		if (!gw_set_defaults_balance(srv, &s->gw, du)) {
			return HANDLER_ERROR;
		}

		/* disable check-local for all exts (default enabled) */
		if (s->gw.exts) { /*(check after gw_set_defaults_backend())*/
			for (size_t j = 0; j < s->gw.exts->used; ++j) {
				gw_extension *ex = s->gw.exts->exts[j];
				for (size_t n = 0; n < ex->used; ++n) {
					ex->hosts[n]->check_local = 0;
				}
			}
		}

		if (!array_is_kvany(s->forwarded_params)) {
			log_error_write(srv, __FILE__, __LINE__, "s",
					"unexpected value for proxy.forwarded; expected ( \"param\" => \"value\" )");
			return HANDLER_ERROR;
		}
		for (size_t j = 0, used = s->forwarded_params->used; j < used; ++j) {
			proxy_forwarded_t param;
			du = s->forwarded_params->data[j];
			if (buffer_is_equal_string(du->key, CONST_STR_LEN("by"))) {
				param = PROXY_FORWARDED_BY;
			} else if (buffer_is_equal_string(du->key, CONST_STR_LEN("for"))) {
				param = PROXY_FORWARDED_FOR;
			} else if (buffer_is_equal_string(du->key, CONST_STR_LEN("host"))) {
				param = PROXY_FORWARDED_HOST;
			} else if (buffer_is_equal_string(du->key, CONST_STR_LEN("proto"))) {
				param = PROXY_FORWARDED_PROTO;
			} else if (buffer_is_equal_string(du->key, CONST_STR_LEN("remote_user"))) {
				param = PROXY_FORWARDED_REMOTE_USER;
			} else {
				log_error_write(srv, __FILE__, __LINE__, "sb",
					        "proxy.forwarded keys must be one of: by, for, host, proto, remote_user, but not:", du->key);
				return HANDLER_ERROR;
			}
			if (du->type == TYPE_STRING) {
				data_string *ds = (data_string *)du;
				if (buffer_is_equal_string(ds->value, CONST_STR_LEN("enable"))) {
					s->forwarded |= param;
				} else if (!buffer_is_equal_string(ds->value, CONST_STR_LEN("disable"))) {
					log_error_write(srv, __FILE__, __LINE__, "sb",
						        "proxy.forwarded values must be one of: 0, 1, enable, disable; error for key:", du->key);
					return HANDLER_ERROR;
				}
			} else if (du->type == TYPE_INTEGER) {
				data_integer *di = (data_integer *)du;
				if (di->value) s->forwarded |= param;
			} else {
				log_error_write(srv, __FILE__, __LINE__, "sb",
					        "proxy.forwarded values must be one of: 0, 1, enable, disable; error for key:", du->key);
				return HANDLER_ERROR;
			}
		}

		if (!array_is_kvany(s->header_params)) {
			log_error_write(srv, __FILE__, __LINE__, "s",
					"unexpected value for proxy.header; expected ( \"param\" => ( \"key\" => \"value\" ) )");
			return HANDLER_ERROR;
		}
		for (size_t j = 0, used = s->header_params->used; j < used; ++j) {
			data_array *da = (data_array *)s->header_params->data[j];
			if (buffer_is_equal_string(da->key, CONST_STR_LEN("https-remap"))) {
				data_string *ds = (data_string *)da;
				if (ds->type != TYPE_STRING) {
					log_error_write(srv, __FILE__, __LINE__, "s",
							"unexpected value for proxy.header; expected \"enable\" or \"disable\" for https-remap");
					return HANDLER_ERROR;
				}
				s->header.https_remap = !buffer_is_equal_string(ds->value, CONST_STR_LEN("disable"))
						     && !buffer_is_equal_string(ds->value, CONST_STR_LEN("0"));
				continue;
			}
			else if (buffer_is_equal_string(da->key, CONST_STR_LEN("upgrade"))) {
				data_string *ds = (data_string *)da;
				if (ds->type != TYPE_STRING) {
					log_error_write(srv, __FILE__, __LINE__, "s",
							"unexpected value for proxy.header; expected \"upgrade\" => \"enable\" or \"disable\"");
					return HANDLER_ERROR;
				}
				s->header.upgrade = !buffer_is_equal_string(ds->value, CONST_STR_LEN("disable"))
						 && !buffer_is_equal_string(ds->value, CONST_STR_LEN("0"));
				continue;
			}
			if (da->type != TYPE_ARRAY || !array_is_kvstring(da->value)) {
				log_error_write(srv, __FILE__, __LINE__, "sb",
						"unexpected value for proxy.header; expected ( \"param\" => ( \"key\" => \"value\" ) ) near key", da->key);
				return HANDLER_ERROR;
			}
			if (buffer_is_equal_string(da->key, CONST_STR_LEN("map-urlpath"))) {
				s->header.urlpaths = da->value;
			}
			else if (buffer_is_equal_string(da->key, CONST_STR_LEN("map-host-request"))) {
				s->header.hosts_request = da->value;
			}
			else if (buffer_is_equal_string(da->key, CONST_STR_LEN("map-host-response"))) {
				s->header.hosts_response = da->value;
			}
			else {
				log_error_write(srv, __FILE__, __LINE__, "sb",
						"unexpected key for proxy.header; expected ( \"param\" => ( \"key\" => \"value\" ) ) near key", da->key);
				return HANDLER_ERROR;
			}
		}
	}

	for (i = 0; i < srv->srvconf.modules->used; i++) {
		data_string *ds = (data_string *)srv->srvconf.modules->data[i];
		if (buffer_is_equal_string(ds->value, CONST_STR_LEN("mod_extforward"))) {
			proxy_check_extforward = 1;
			break;
		}
	}

	return HANDLER_GO_ON;
}


/* (future: might move to http-header-glue.c) */
static const buffer * http_header_remap_host_match (buffer *b, size_t off, http_header_remap_opts *remap_hdrs, int is_req, size_t alen)
{
    const array *hosts = is_req
      ? remap_hdrs->hosts_request
      : remap_hdrs->hosts_response;
    if (hosts) {
        const char * const s = b->ptr+off;
        for (size_t i = 0, used = hosts->used; i < used; ++i) {
            const data_string * const ds = (data_string *)hosts->data[i];
            const buffer *k = ds->key;
            size_t mlen = buffer_string_length(k);
            if (1 == mlen && k->ptr[0] == '-') {
                /* match with authority provided in Host (if is_req)
                 * (If no Host in client request, then matching against empty
                 *  string will probably not match, and no remap will be
                 *  performed) */
                k = is_req
                  ? remap_hdrs->http_host
                  : remap_hdrs->forwarded_host;
                if (NULL == k) continue;
                mlen = buffer_string_length(k);
            }
            if (mlen == alen && 0 == strncasecmp(s, k->ptr, alen)) {
                if (buffer_is_equal_string(ds->value, CONST_STR_LEN("-"))) {
                    return remap_hdrs->http_host;
                }
                else if (!buffer_string_is_empty(ds->value)) {
                    /*(save first matched request host for response match)*/
                    if (is_req && NULL == remap_hdrs->forwarded_host)
                        remap_hdrs->forwarded_host = ds->value;
                    return ds->value;
                } /*(else leave authority as-is and stop matching)*/
                break;
            }
        }
    }
    return NULL;
}


/* (future: might move to http-header-glue.c) */
static size_t http_header_remap_host (buffer *b, size_t off, http_header_remap_opts *remap_hdrs, int is_req, size_t alen)
{
    const buffer * const m =
      http_header_remap_host_match(b, off, remap_hdrs, is_req, alen);
    if (NULL == m) return alen; /*(no match; return original authority length)*/

    buffer_substr_replace(b, off, alen, m);
    return buffer_string_length(m); /*(length of replacement authority)*/
}


/* (future: might move to http-header-glue.c) */
static void http_header_remap_urlpath (buffer *b, size_t off, http_header_remap_opts *remap_hdrs, int is_req)
{
    const array *urlpaths = remap_hdrs->urlpaths;
    if (urlpaths) {
        const char * const s = b->ptr+off;
        const size_t plen = buffer_string_length(b) - off; /*(urlpath len)*/
        if (is_req) { /* request */
            for (size_t i = 0, used = urlpaths->used; i < used; ++i) {
                const data_string * const ds = (data_string *)urlpaths->data[i];
                const size_t mlen = buffer_string_length(ds->key);
                if (mlen <= plen && 0 == memcmp(s, ds->key->ptr, mlen)) {
                    if (NULL == remap_hdrs->forwarded_urlpath)
                        remap_hdrs->forwarded_urlpath = ds;
                    buffer_substr_replace(b, off, mlen, ds->value);
                    break;
                }
            }
        }
        else {        /* response; perform reverse map */
            if (NULL != remap_hdrs->forwarded_urlpath) {
                const data_string * const ds = remap_hdrs->forwarded_urlpath;
                const size_t mlen = buffer_string_length(ds->value);
                if (mlen <= plen && 0 == memcmp(s, ds->value->ptr, mlen)) {
                    buffer_substr_replace(b, off, mlen, ds->key);
                    return;
                }
            }
            for (size_t i = 0, used = urlpaths->used; i < used; ++i) {
                const data_string * const ds = (data_string *)urlpaths->data[i];
                const size_t mlen = buffer_string_length(ds->value);
                if (mlen <= plen && 0 == memcmp(s, ds->value->ptr, mlen)) {
                    buffer_substr_replace(b, off, mlen, ds->key);
                    break;
                }
            }
        }
    }
}


/* (future: might move to http-header-glue.c) */
static void http_header_remap_uri (buffer *b, size_t off, http_header_remap_opts *remap_hdrs, int is_req)
{
    /* find beginning of URL-path (might be preceded by scheme://authority
     * (caller should make sure any leading whitespace is prior to offset) */
    if (b->ptr[off] != '/') {
        char *s = b->ptr+off;
        size_t alen; /*(authority len (host len))*/
        size_t slen; /*(scheme len)*/
        const buffer *m;
        /* skip over scheme and authority of URI to find beginning of URL-path
         * (value might conceivably be relative URL-path instead of URI) */
        if (NULL == (s = strchr(s, ':')) || s[1] != '/' || s[2] != '/') return;
        slen = s - (b->ptr+off);
        s += 3;
        off = (size_t)(s - b->ptr);
        if (NULL != (s = strchr(s, '/'))) {
            alen = (size_t)(s - b->ptr) - off;
            if (0 == alen) return; /*(empty authority, e.g. "http:///")*/
        }
        else {
            alen = buffer_string_length(b) - off;
            if (0 == alen) return; /*(empty authority, e.g. "http:///")*/
            buffer_append_string_len(b, CONST_STR_LEN("/"));
        }

        /* remap authority (if configured) and set offset to url-path */
        m = http_header_remap_host_match(b, off, remap_hdrs, is_req, alen);
        if (NULL != m) {
            if (remap_hdrs->https_remap
                && (is_req ? 5==slen && 0==memcmp(b->ptr+off-slen-3,"https",5)
                           : 4==slen && 0==memcmp(b->ptr+off-slen-3,"http",4))){
                if (is_req) {
                    memcpy(b->ptr+off-slen-3+4,"://",3);  /*("https"=>"http")*/
                    --off;
                    ++alen;
                }
                else {/*(!is_req)*/
                    memcpy(b->ptr+off-slen-3+4,"s://",4); /*("http" =>"https")*/
                    ++off;
                    --alen;
                }
            }
            buffer_substr_replace(b, off, alen, m);
            alen = buffer_string_length(m);/*(length of replacement authority)*/
        }
        off += alen;
    }

    /* remap URLs (if configured) */
    http_header_remap_urlpath(b, off, remap_hdrs, is_req);
}


/* (future: might move to http-header-glue.c) */
static void http_header_remap_setcookie (buffer *b, size_t off, http_header_remap_opts *remap_hdrs)
{
    /* Given the special-case of Set-Cookie and the (too) loosely restricted
     * characters allowed, for best results, the Set-Cookie value should be the
     * entire string in b from offset to end of string.  In response headers,
     * lighttpd may concatenate multiple Set-Cookie headers into single entry
     * in con->response.headers, separated by "\r\nSet-Cookie: " */
    for (char *s, *n = b->ptr+off; (s = n); ) {
        size_t len;
        n = strchr(s, '\n');
        if (NULL == n) {
            len = (size_t)(b->ptr + buffer_string_length(b) - s);
        }
        else {
            len = (size_t)(n - s);
            n += sizeof("Set-Cookie: "); /*(include +1 for '\n')*/
        }
        for (char *e = s; NULL != (s = memchr(e, ';', len)); ) {
            do { ++s; } while (*s == ' ' || *s == '\t');
            if ('\0' == *s) return;
            /*(interested only in Domain and Path attributes)*/
            e = memchr(s, '=', len - (size_t)(s - e));
            if (NULL == e) { e = s+1; continue; }
            ++e;
            switch ((int)(e - s - 1)) {
              case 4:
                if (0 == strncasecmp(s, "path", 4)) {
                    if (*e == '"') ++e;
                    if (*e != '/') continue;
                    off = (size_t)(e - b->ptr);
                    http_header_remap_urlpath(b, off, remap_hdrs, 0);
                    e = b->ptr+off; /*(b may have been reallocated)*/
                    continue;
                }
                break;
              case 6:
                if (0 == strncasecmp(s, "domain", 6)) {
                    size_t alen = 0;
                    if (*e == '"') ++e;
                    if (*e == '.') ++e;
                    if (*e == ';') continue;
                    off = (size_t)(e - b->ptr);
                    for (char c; (c = e[alen]) != ';' && c != ' ' && c != '\t'
                                          && c != '\r' && c != '\0'; ++alen);
                    len = http_header_remap_host(b, off, remap_hdrs, 0, alen);
                    e = b->ptr+off+len; /*(b may have been reallocated)*/
                    continue;
                }
                break;
              default:
                break;
            }
        }
    }
}


static void proxy_append_header(connection *con, const char *key, const size_t klen, const char *value, const size_t vlen) {
	data_string *ds_dst;

	if (NULL == (ds_dst = (data_string *)array_get_unused_element(con->request.headers, TYPE_STRING))) {
		ds_dst = data_string_init();
	}

	buffer_copy_string_len(ds_dst->key, key, klen);
	buffer_copy_string_len(ds_dst->value, value, vlen);
	array_insert_unique(con->request.headers, (data_unset *)ds_dst);
}

static void buffer_append_string_backslash_escaped(buffer *b, const char *s, size_t len) {
    /* (future: might move to buffer.c) */
    size_t j = 0;
    char *p;

    buffer_string_prepare_append(b, len*2 + 4);
    p = b->ptr + buffer_string_length(b);

    for (size_t i = 0; i < len; ++i) {
        int c = s[i];
        if (c == '"' || c == '\\' || c == 0x7F || (c < 0x20 && c != '\t'))
            p[j++] = '\\';
        p[j++] = c;
    }

    buffer_commit(b, j);
}

static void proxy_set_Forwarded(connection *con, const unsigned int flags) {
    data_string *ds = NULL, *dsfor = NULL, *dsproto = NULL, *dshost = NULL;
    buffer *b;
    int semicolon = 0;

    if (proxy_check_extforward) {
        dsfor   = (data_string *)
          array_get_element(con->environment, "_L_EXTFORWARD_ACTUAL_FOR");
        dsproto = (data_string *)
          array_get_element(con->environment, "_L_EXTFORWARD_ACTUAL_PROTO");
        dshost  = (data_string *)
          array_get_element(con->environment, "_L_EXTFORWARD_ACTUAL_HOST");
    }

    /* note: set "Forwarded" prior to updating X-Forwarded-For (below) */

    if (flags)
        ds = (data_string *)
          array_get_element(con->request.headers, "Forwarded");

    if (flags && NULL == ds) {
        data_string *xff;
        ds = (data_string *)
          array_get_unused_element(con->request.headers, TYPE_STRING);
        if (NULL == ds) ds = data_string_init();
        buffer_copy_string_len(ds->key, CONST_STR_LEN("Forwarded"));
        array_insert_unique(con->request.headers, (data_unset *)ds);
        xff = (data_string *)
          array_get_element(con->request.headers, "X-Forwarded-For");
        if (NULL != xff && !buffer_string_is_empty(xff->value)) {
            /* use X-Forwarded-For contents to seed Forwarded */
            char *s = xff->value->ptr;
            size_t used = buffer_string_length(xff->value);
            for (size_t i=0, j, ipv6; i < used; ++i) {
                while (s[i] == ' ' || s[i] == '\t' || s[i] == ',') ++i;
                if (s[i] == '\0') break;
                j = i;
                do {
                    ++i;
                } while (s[i]!=' ' && s[i]!='\t' && s[i]!=',' && s[i]!='\0');
                buffer_append_string_len(ds->value, CONST_STR_LEN("for="));
                /* over-simplified test expecting only IPv4 or IPv6 addresses,
                 * (not expecting :port, so treat existence of colon as IPv6,
                 *  and not expecting unix paths, especially not containing ':')
                 * quote all strings, backslash-escape since IPs not validated*/
                ipv6 = (NULL != memchr(s+j, ':', i-j)); /*(over-simplified) */
                buffer_append_string_len(ds->value, CONST_STR_LEN("\""));
                if (ipv6)
                    buffer_append_string_len(ds->value, CONST_STR_LEN("["));
                buffer_append_string_backslash_escaped(ds->value, s+j, i-j);
                if (ipv6)
                    buffer_append_string_len(ds->value, CONST_STR_LEN("]"));
                buffer_append_string_len(ds->value, CONST_STR_LEN("\""));
                buffer_append_string_len(ds->value, CONST_STR_LEN(", "));
            }
        }
    } else if (flags) { /*(NULL != ds)*/
        buffer_append_string_len(ds->value, CONST_STR_LEN(", "));
    }

    if (flags & PROXY_FORWARDED_FOR) {
        buffer_append_string_len(ds->value, CONST_STR_LEN("for="));
        if (NULL != dsfor) {
            /* over-simplified test expecting only IPv4 or IPv6 addresses,
             * (not expecting :port, so treat existence of colon as IPv6,
             *  and not expecting unix paths, especially not containing ':')
             * quote all strings and backslash-escape since IPs not validated
             * (should be IP from original con->dst_addr_buf,
             *  so trustable and without :port) */
            int ipv6 = (NULL != strchr(dsfor->value->ptr, ':'));
            buffer_append_string_len(ds->value, CONST_STR_LEN("\""));
            if (ipv6) buffer_append_string_len(ds->value, CONST_STR_LEN("["));
            buffer_append_string_backslash_escaped(
              ds->value, CONST_BUF_LEN(dsfor->value));
            if (ipv6) buffer_append_string_len(ds->value, CONST_STR_LEN("]"));
            buffer_append_string_len(ds->value, CONST_STR_LEN("\""));
        } else if (con->dst_addr.plain.sa_family == AF_INET) {
            /*(Note: if :port is added, then must be quoted-string:
             * e.g. for="...:port")*/
            buffer_append_string_buffer(ds->value, con->dst_addr_buf);
      #ifdef HAVE_IPV6
        } else if (con->dst_addr.plain.sa_family == AF_INET6) {
            buffer_append_string_len(ds->value, CONST_STR_LEN("\"["));
            buffer_append_string_buffer(ds->value, con->dst_addr_buf);
            buffer_append_string_len(ds->value, CONST_STR_LEN("]\""));
      #endif
        } else {
            buffer_append_string_len(ds->value, CONST_STR_LEN("\""));
            buffer_append_string_backslash_escaped(
              ds->value, CONST_BUF_LEN(con->dst_addr_buf));
            buffer_append_string_len(ds->value, CONST_STR_LEN("\""));
        }
        semicolon = 1;
    }

    if (flags & PROXY_FORWARDED_BY) {
        /* Note: getsockname() and inet_ntop() are expensive operations.
         * (recommendation: do not to enable by=... unless required)
         * future: might use con->srv_socket->srv_token if addr is not
         *   INADDR_ANY or in6addr_any, but must omit optional :port
         *   from con->srv_socket->srv_token for consistency */
        sock_addr *addr = &con->srv_socket->addr;
        sock_addr addrbuf;
        socklen_t addrlen = sizeof(addrbuf);

        if (semicolon) buffer_append_string_len(ds->value, CONST_STR_LEN(";"));
        buffer_append_string_len(ds->value, CONST_STR_LEN("by="));
        buffer_append_string_len(ds->value, CONST_STR_LEN("\""));
        if (addr->plain.sa_family == AF_INET) {
            if (0==getsockname(con->fd,(struct sockaddr *)&addrbuf,&addrlen)) {
                sock_addr_inet_ntop_append_buffer(ds->value, &addrbuf);
            }
            buffer_append_string_len(ds->value, CONST_STR_LEN(":"));
            buffer_append_int(ds->value, ntohs(addr->ipv4.sin_port));
      #ifdef HAVE_IPV6
        } else if (addr->plain.sa_family == AF_INET6) {
            if (0 == getsockname(con->fd,(struct sockaddr *)&addrbuf,&addrlen)){
                buffer_append_string_len(ds->value, CONST_STR_LEN("["));
                sock_addr_inet_ntop_append_buffer(ds->value, &addrbuf);
                buffer_append_string_len(ds->value, CONST_STR_LEN("]"));
                buffer_append_string_len(ds->value, CONST_STR_LEN(":"));
                buffer_append_int(ds->value, ntohs(addr->ipv6.sin6_port));
            }
      #endif
      #ifdef HAVE_SYS_UN_H
        } else if (addr->plain.sa_family == AF_UNIX) {
            buffer_append_string_backslash_escaped(
              ds->value, CONST_BUF_LEN(con->srv_socket->srv_token));
      #endif
        }
        buffer_append_string_len(ds->value, CONST_STR_LEN("\""));
        semicolon = 1;
    }

    if (flags & PROXY_FORWARDED_PROTO) {
        /* expecting "http" or "https"
         * (not checking if quoted-string and encoding needed) */
        if (semicolon) buffer_append_string_len(ds->value, CONST_STR_LEN(";"));
        buffer_append_string_len(ds->value, CONST_STR_LEN("proto="));
        if (NULL != dsproto) {
            buffer_append_string_buffer(ds->value, dsproto->value);
        } else if (con->srv_socket->is_ssl) {
            buffer_append_string_len(ds->value, CONST_STR_LEN("https"));
        } else {
            buffer_append_string_len(ds->value, CONST_STR_LEN("http"));
        }
        semicolon = 1;
    }

    if (flags & PROXY_FORWARDED_HOST) {
        if (NULL != dshost) {
            if (semicolon)
                buffer_append_string_len(ds->value, CONST_STR_LEN(";"));
            buffer_append_string_len(ds->value, CONST_STR_LEN("host=\""));
            buffer_append_string_backslash_escaped(
              ds->value, CONST_BUF_LEN(dshost->value));
            buffer_append_string_len(ds->value, CONST_STR_LEN("\""));
            semicolon = 1;
        } else if (!buffer_string_is_empty(con->request.http_host)) {
            if (semicolon)
                buffer_append_string_len(ds->value, CONST_STR_LEN(";"));
            buffer_append_string_len(ds->value, CONST_STR_LEN("host=\""));
            buffer_append_string_backslash_escaped(
              ds->value, CONST_BUF_LEN(con->request.http_host));
            buffer_append_string_len(ds->value, CONST_STR_LEN("\""));
            semicolon = 1;
        }
    }

    if (flags & PROXY_FORWARDED_REMOTE_USER) {
        data_string *remote_user = (data_string *)
          array_get_element(con->environment, "REMOTE_USER");
        if (NULL != remote_user) {
            if (semicolon)
                buffer_append_string_len(ds->value, CONST_STR_LEN(";"));
            buffer_append_string_len(ds->value,CONST_STR_LEN("remote_user=\""));
            buffer_append_string_backslash_escaped(
              ds->value, CONST_BUF_LEN(remote_user->value));
            buffer_append_string_len(ds->value,CONST_STR_LEN("\""));
            semicolon = 1;
        }
    }

    /* legacy X-* headers, including X-Forwarded-For */

    b = (NULL != dsfor) ? dsfor->value : con->dst_addr_buf;
    proxy_append_header(con, CONST_STR_LEN("X-Forwarded-For"),
                             CONST_BUF_LEN(b));

    b = (NULL != dshost) ? dshost->value : con->request.http_host;
    if (!buffer_string_is_empty(b)) {
        proxy_append_header(con, CONST_STR_LEN("X-Host"),
                                 CONST_BUF_LEN(b));
        proxy_append_header(con, CONST_STR_LEN("X-Forwarded-Host"),
                                 CONST_BUF_LEN(b));
    }

    b = (NULL != dsproto) ? dsproto->value : con->uri.scheme;
    proxy_append_header(con, CONST_STR_LEN("X-Forwarded-Proto"),
                             CONST_BUF_LEN(b));
}


static handler_t proxy_create_env(server *srv, gw_handler_ctx *gwhctx) {
	handler_ctx *hctx = (handler_ctx *)gwhctx;
	connection *con = hctx->gw.remote_conn;
	buffer *b = buffer_init();
	const int remap_headers = (NULL != hctx->remap_hdrs.urlpaths
				   || NULL != hctx->remap_hdrs.hosts_request);
	const int upgrade = hctx->remap_hdrs.upgrade
			    && (NULL != array_get_element(con->request.headers, "Upgrade"));
	buffer_string_prepare_copy(b, 8192-1);

	/* build header */

	/* request line */
	buffer_copy_string(b, get_http_method_name(con->request.http_method));
	buffer_append_string_len(b, CONST_STR_LEN(" "));
	buffer_append_string_buffer(b, con->request.uri);
	if (remap_headers)
		http_header_remap_uri(b, buffer_string_length(b) - buffer_string_length(con->request.uri), &hctx->remap_hdrs, 1);
	if (!upgrade)
		buffer_append_string_len(b, CONST_STR_LEN(" HTTP/1.0\r\n"));
	else
		buffer_append_string_len(b, CONST_STR_LEN(" HTTP/1.1\r\n"));

	if (hctx->conf.replace_http_host && !buffer_string_is_empty(hctx->gw.host->id)) {
		if (hctx->gw.conf.debug > 1) {
			log_error_write(srv, __FILE__, __LINE__,  "SBS",
					"proxy - using \"", hctx->gw.host->id, "\" as HTTP Host");
		}
		buffer_append_string_len(b, CONST_STR_LEN("Host: "));
		buffer_append_string_buffer(b, hctx->gw.host->id);
		buffer_append_string_len(b, CONST_STR_LEN("\r\n"));
	} else if (!buffer_string_is_empty(con->request.http_host)) {
		buffer_append_string_len(b, CONST_STR_LEN("Host: "));
		buffer_append_string_buffer(b, con->request.http_host);
		if (remap_headers) {
			size_t alen = buffer_string_length(con->request.http_host);
			http_header_remap_host(b, buffer_string_length(b) - alen, &hctx->remap_hdrs, 1, alen);
		}
		buffer_append_string_len(b, CONST_STR_LEN("\r\n"));
	}

	/* "Forwarded" and legacy X- headers */
	proxy_set_Forwarded(con, hctx->conf.forwarded);

	if (HTTP_METHOD_GET != con->request.http_method
	    && HTTP_METHOD_HEAD != con->request.http_method
	    && con->request.content_length >= 0) {
		/* set Content-Length if client sent Transfer-Encoding: chunked
		 * and not streaming to backend (request body has been fully received) */
		data_string *ds = (data_string *) array_get_element(con->request.headers, "Content-Length");
		if (NULL == ds || buffer_string_is_empty(ds->value)) {
			char buf[LI_ITOSTRING_LENGTH];
			li_itostrn(buf, sizeof(buf), con->request.content_length);
			if (NULL == ds) {
				proxy_append_header(con, CONST_STR_LEN("Content-Length"), buf, strlen(buf));
			} else {
				buffer_copy_string(ds->value, buf);
			}
		}
	}

	/* request header */
	for (size_t i = 0, used = con->request.headers->used; i < used; ++i) {
		data_string *ds = (data_string *)con->request.headers->data[i];
		const size_t klen = buffer_string_length(ds->key);
		size_t vlen;
		switch (klen) {
		default:
			break;
		case 4:
			if (buffer_is_equal_caseless_string(ds->key, CONST_STR_LEN("Host"))) continue; /*(handled further above)*/
			break;
		case 10:
			if (buffer_is_equal_caseless_string(ds->key, CONST_STR_LEN("Connection"))) continue;
			if (buffer_is_equal_caseless_string(ds->key, CONST_STR_LEN("Set-Cookie"))) continue; /*(response header only; avoid accidental reflection)*/
			break;
		case 16:
			if (buffer_is_equal_caseless_string(ds->key, CONST_STR_LEN("Proxy-Connection"))) continue;
			break;
		case 5:
			/* Do not emit HTTP_PROXY in environment.
			 * Some executables use HTTP_PROXY to configure
			 * outgoing proxy.  See also https://httpoxy.org/ */
			if (buffer_is_equal_caseless_string(ds->key, CONST_STR_LEN("Proxy"))) continue;
			break;
		case 0:
			continue;
		}

		vlen = buffer_string_length(ds->value);
		if (0 == vlen) continue;

		buffer_append_string_len(b, ds->key->ptr, klen);
		buffer_append_string_len(b, CONST_STR_LEN(": "));
		buffer_append_string_len(b, ds->value->ptr, vlen);
		buffer_append_string_len(b, CONST_STR_LEN("\r\n"));

		if (!remap_headers) continue;

		/* check for hdrs for which to remap URIs in-place after append to b */

		switch (klen) {
		default:
			continue;
	      #if 0 /* "URI" is HTTP response header (non-standard; historical in Apache) */
		case 3:
			if (buffer_is_equal_caseless_string(ds->key, CONST_STR_LEN("URI"))) break;
			continue;
	      #endif
	      #if 0 /* "Location" is HTTP response header */
		case 8:
			if (buffer_is_equal_caseless_string(ds->key, CONST_STR_LEN("Location"))) break;
			continue;
	      #endif
		case 11: /* "Destination" is WebDAV request header */
			if (buffer_is_equal_caseless_string(ds->key, CONST_STR_LEN("Destination"))) break;
			continue;
		case 16: /* "Content-Location" may be HTTP request or response header */
			if (buffer_is_equal_caseless_string(ds->key, CONST_STR_LEN("Content-Location"))) break;
			continue;
		}

		http_header_remap_uri(b, buffer_string_length(b) - vlen - 2, &hctx->remap_hdrs, 1);
	}

	if (!upgrade)
		buffer_append_string_len(b, CONST_STR_LEN("Connection: close\r\n\r\n"));
	else
		buffer_append_string_len(b, CONST_STR_LEN("Connection: close, upgrade\r\n\r\n"));

	hctx->gw.wb_reqlen = buffer_string_length(b);
	chunkqueue_append_buffer(hctx->gw.wb, b);
	buffer_free(b);

	if (con->request.content_length) {
		chunkqueue_append_chunkqueue(hctx->gw.wb, con->request_content_queue);
		if (con->request.content_length > 0)
			hctx->gw.wb_reqlen += con->request.content_length; /* total req size */
		else /* as-yet-unknown total request size (Transfer-Encoding: chunked)*/
			hctx->gw.wb_reqlen = -hctx->gw.wb_reqlen;
	}

	status_counter_inc(srv, CONST_STR_LEN("proxy.requests"));
	return HANDLER_GO_ON;
}

#define PATCH(x) \
	p->conf.x = s->x;
#define PATCH_GW(x) \
	p->conf.gw.x = s->gw.x;
static int mod_proxy_patch_connection(server *srv, connection *con, plugin_data *p) {
	size_t i, j;
	plugin_config *s = p->config_storage[0];

	PATCH_GW(exts);
	PATCH_GW(exts_auth);
	PATCH_GW(exts_resp);
	PATCH_GW(debug);
	PATCH_GW(ext_mapping);
	PATCH_GW(balance);
	PATCH(replace_http_host);
	PATCH(forwarded);
	PATCH(header); /*(copies struct)*/

	/* skip the first, the global context */
	for (i = 1; i < srv->config_context->used; i++) {
		data_config *dc = (data_config *)srv->config_context->data[i];
		s = p->config_storage[i];

		/* condition didn't match */
		if (!config_check_cond(srv, con, dc)) continue;

		/* merge config */
		for (j = 0; j < dc->value->used; j++) {
			data_unset *du = dc->value->data[j];

			if (buffer_is_equal_string(du->key, CONST_STR_LEN("proxy.server"))) {
				PATCH_GW(exts);
				PATCH_GW(exts_auth);
				PATCH_GW(exts_resp);
			} else if (buffer_is_equal_string(du->key, CONST_STR_LEN("proxy.debug"))) {
				PATCH_GW(debug);
			} else if (buffer_is_equal_string(du->key, CONST_STR_LEN("proxy.balance"))) {
				PATCH_GW(balance);
			} else if (buffer_is_equal_string(du->key, CONST_STR_LEN("proxy.map-extensions"))) {
				PATCH_GW(ext_mapping);
			} else if (buffer_is_equal_string(du->key, CONST_STR_LEN("proxy.replace-http-host"))) {
				PATCH(replace_http_host);
			} else if (buffer_is_equal_string(du->key, CONST_STR_LEN("proxy.forwarded"))) {
				PATCH(forwarded);
			} else if (buffer_is_equal_string(du->key, CONST_STR_LEN("proxy.header"))) {
				PATCH(header); /*(copies struct)*/
			}
		}
	}

	return 0;
}
#undef PATCH_GW
#undef PATCH

static handler_t proxy_response_headers(server *srv, connection *con, struct http_response_opts_t *opts) {
    /* response headers just completed */
    handler_ctx *hctx = (handler_ctx *)opts->pdata;

    if (con->parsed_response & HTTP_UPGRADE) {
        if (hctx->remap_hdrs.upgrade && con->http_status == 101) {
            /* 101 Switching Protocols; transition to transparent proxy */
            gw_set_transparent(srv, &hctx->gw);
            http_response_upgrade_read_body_unknown(srv, con);
        }
        else {
            con->parsed_response &= ~HTTP_UPGRADE;
          #if 0
            /* preserve prior questionable behavior; likely broken behavior
             * anyway if backend thinks connection is being upgraded but client
             * does not receive Connection: upgrade */
            response_header_overwrite(srv, con, CONST_STR_LEN("Upgrade"),
                                                CONST_STR_LEN(""));
          #endif
        }
    }

    /* rewrite paths, if needed */

    if (NULL == hctx->remap_hdrs.urlpaths
        && NULL == hctx->remap_hdrs.hosts_response)
        return HANDLER_GO_ON;

    if (con->parsed_response & HTTP_LOCATION) {
        data_string *ds = (data_string *)
          array_get_element(con->response.headers, "Location");
        if (ds) http_header_remap_uri(ds->value, 0, &hctx->remap_hdrs, 0);
    }
    if (con->parsed_response & HTTP_CONTENT_LOCATION) {
        data_string *ds = (data_string *)
          array_get_element(con->response.headers, "Content-Location");
        if (ds) http_header_remap_uri(ds->value, 0, &hctx->remap_hdrs, 0);
    }
    if (con->parsed_response & HTTP_SET_COOKIE) {
        data_string *ds = (data_string *)
          array_get_element(con->response.headers, "Set-Cookie");
        if (ds) http_header_remap_setcookie(ds->value, 0, &hctx->remap_hdrs);
    }

    return HANDLER_GO_ON;
}

static handler_t mod_proxy_check_extension(server *srv, connection *con, void *p_d) {
	plugin_data *p = p_d;
	handler_t rc;

	if (con->mode != DIRECT) return HANDLER_GO_ON;

	mod_proxy_patch_connection(srv, con, p);
	if (NULL == p->conf.gw.exts) return HANDLER_GO_ON;

	rc = gw_check_extension(srv, con, (gw_plugin_data *)p, 1, sizeof(handler_ctx));
	if (HANDLER_GO_ON != rc) return rc;

	if (con->mode == p->id) {
		handler_ctx *hctx = con->plugin_ctx[p->id];
		hctx->gw.create_env = proxy_create_env;
		hctx->gw.response = buffer_init();
		hctx->gw.opts.backend = BACKEND_PROXY;
		hctx->gw.opts.pdata = hctx;
		hctx->gw.opts.headers = proxy_response_headers;

		hctx->remap_hdrs           = p->conf.header; /*(copies struct)*/
		hctx->remap_hdrs.http_host = con->request.http_host;
		hctx->remap_hdrs.upgrade  &= (con->request.http_version == HTTP_VERSION_1_1);
		/* mod_proxy currently sends all backend requests as http.
		 * https-remap is a flag since it might not be needed if backend
		 * honors Forwarded or X-Forwarded-Proto headers, e.g. by using
		 * lighttpd mod_extforward or similar functionality in backend*/
		if (hctx->remap_hdrs.https_remap) {
			hctx->remap_hdrs.https_remap =
			  buffer_is_equal_string(con->uri.scheme, CONST_STR_LEN("https"));
		}
	}

	return HANDLER_GO_ON;
}


int mod_proxy_plugin_init(plugin *p);
int mod_proxy_plugin_init(plugin *p) {
	p->version      = LIGHTTPD_VERSION_ID;
	p->name         = buffer_init_string("proxy");

	p->init         = mod_proxy_init;
	p->cleanup      = mod_proxy_free;
	p->set_defaults = mod_proxy_set_defaults;
	p->connection_reset        = gw_connection_reset;
	p->handle_uri_clean        = mod_proxy_check_extension;
	p->handle_subrequest       = gw_handle_subrequest;
	p->handle_trigger          = gw_handle_trigger;

	p->data         = NULL;

	return 0;
}
