/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * mod_virtualroot.c: support for dynamically configured mass virtual hosting
 *
 * Copyright (c) 2010 Andy Thompson
 *
 * Based on mod_vhost_alias.c
 * Copyright (c) 1998-1999 Demon Internet Ltd.
 *
 * This software was submitted by Demon Internet to the Apache Software Foundation
 * in May 1999. Future revisions and derivatives of this source code
 * must acknowledge Demon Internet as the original contributor of
 * this module. All other licensing and usage conditions are those
 * of the Apache Software Foundation.
 *
 * Originally written by Tony Finch <fanf@demon.net> <dot@dotat.at>.
 *
 * Implementation ideas were taken from mod_alias.c. The overall
 * concept is derived from the OVERRIDE_DOC_ROOT/OVERRIDE_CGIDIR
 * patch to Apache 1.3b3 and a similar feature in Demon's thttpd,
 * both written by James Grinter <jrg@blodwen.demon.co.uk>.
 */

#include "apr.h"
#include "apr_strings.h"
#include "apr_hooks.h"
#include "apr_lib.h"

#define APR_WANT_STRFUNC
#include "apr_want.h"

#define CORE_PRIVATE
#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_request.h"


module AP_MODULE_DECLARE_DATA virtualroot_module;


/*
 * basic configuration things
 * we abbreviate "mod_virtualroot" to "mvr" for shorter names
 */

/*
 * Per-server module config record.
 */
typedef struct mvr_sconf_t {
    const char *doc_root;
    apr_array_header_t *domains;
} mvr_sconf_t;

static void *mvr_create_server_config(apr_pool_t *p, server_rec *s)
{
    mvr_sconf_t *conf;

    conf = (mvr_sconf_t *) apr_pcalloc(p, sizeof(mvr_sconf_t));
    conf->doc_root = NULL;
    conf->domains = apr_array_make(p, 0, sizeof(char *));
    return conf;
}

static void *mvr_merge_server_config(apr_pool_t *p, void *parentv, void *childv)
{
    mvr_sconf_t *parent = (mvr_sconf_t *) parentv;
    mvr_sconf_t *child = (mvr_sconf_t *) childv;
    mvr_sconf_t *conf;

    conf = (mvr_sconf_t *) apr_pcalloc(p, sizeof(*conf));
    if (child->doc_root == NULL) {
        conf->doc_root = parent->doc_root;
    } else {
        conf->doc_root = child->doc_root;
    }
    if (child->domains->nelts == 0) {
        conf->domains = parent->domains;
    } else {
        conf->domains = child->domains;
    }
    return conf;
}

static const char *virtualroot_set(cmd_parms *cmd, void *dummy, const char *map)
{
    mvr_sconf_t *conf;
    const char **pmap;
    const char *p;

    conf = (mvr_sconf_t *) ap_get_module_config(cmd->server->module_config,
                                                &virtualroot_module);

    if (!ap_os_is_path_absolute(cmd->pool, map)) {
        if (strcasecmp(map, "none")) {
            return "format string must be an absolute path, or 'none'";
        }
        return NULL;
    }

    /* sanity check */
    p = map;
    while (*p != '\0') {
        if (*p++ != '%') {
            continue;
        }
        /* we just found a '%' */
        if (*p == 'p' || *p == '%') {
            ++p;
            continue;
        }
        /* optional dash */
        if (*p == '-') {
            ++p;
        }
        /* digit N */
        if (apr_isdigit(*p)) {
            ++p;
        } else {
            return "syntax error in format string";
        }
        /* optional plus */
        if (*p == '+') {
            ++p;
        }
        /* do we end here? */
        if (*p != '.') {
            continue;
        }
        ++p;
        /* optional dash */
        if (*p == '-') {
            ++p;
        }
        /* digit M */
        if (apr_isdigit(*p)) {
            ++p;
        } else {
            return "syntax error in format string";
        }
        /* optional plus */
        if (*p == '+') {
            ++p;
        }
    }
    conf->doc_root = map;
    return NULL;
}

static const char *virtualroot_domain_set(cmd_parms *cmd, void *dummy, const char *domain) {
    server_rec *s = cmd->server;
    mvr_sconf_t *conf = (mvr_sconf_t *) ap_get_module_config(cmd->server->module_config,
                                                            &virtualroot_module);

    *(char **)apr_array_push(conf->domains) = apr_pstrdup(cmd->pool, domain);
    return NULL;
}

static const command_rec mvr_commands[] =
{
    AP_INIT_TAKE1("VirtualRoot", virtualroot_set,
                  NULL, RSRC_CONF,
                  "VirtualRoot Path"),
    AP_INIT_ITERATE("VirtualRootDomain", virtualroot_domain_set,
                    NULL, RSRC_CONF,
                    "VirtualRootDomain Domain1 ... [DomainN]"),
    { NULL }
};


/*
 * This really wants to be a nested function
 * but C is too feeble to support them.
 */
static APR_INLINE void virtualroot_checkspace(request_rec *r, char *buf,
                                             char **pdest, int size, char **pdocroot)
{
    /* XXX: what if size > HUGE_STRING_LEN? */
    if (*pdest + size > buf + HUGE_STRING_LEN) {
        **pdest = '\0';
        if (r->filename) {
            *pdocroot = apr_pstrcat(r->pool, r->filename, buf, NULL);
        }
        else {
            *pdocroot = apr_pstrdup(r->pool, buf);
        }
        *pdest = buf;
    }
}

static const char *virtualroot_interpolate(request_rec *r, const char *name,
                                    const char *map)
{
    /* 0..9 9..0 */
    enum { MAXDOTS = 19 };
    const char *dots[MAXDOTS+1];
    int ndots;

    char buf[HUGE_STRING_LEN];
    char *dest, last, *docroot;

    int N, M, Np, Mp, Nd, Md;
    const char *start, *end;

    const char *p;
    apr_finfo_t sb;

    ndots = 0;
    dots[ndots++] = name-1; /* slightly naughty */
    for (p = name; *p; ++p){
        if (*p == '.' && ndots < MAXDOTS) {
            dots[ndots++] = p;
        }
    }
    dots[ndots] = p;

    docroot = NULL;

    dest = buf;
    last = '\0';
    while (*map) {
        if (*map != '%') {
            /* normal characters */
            virtualroot_checkspace(r, buf, &dest, 1, &docroot);
            last = *dest++ = *map++;
            continue;
        }
        /* we are in a format specifier */
        ++map;
        /* can't be a slash */
        last = '\0';
        /* %% -> % */
        if (*map == '%') {
            ++map;
            virtualroot_checkspace(r, buf, &dest, 1, &docroot);
            *dest++ = '%';
            continue;
        }
        /* port number */
        if (*map == 'p') {
            ++map;
            /* no. of decimal digits in a short plus one */
            virtualroot_checkspace(r, buf, &dest, 7, &docroot);
            dest += apr_snprintf(dest, 7, "%d", ap_get_server_port(r));
            continue;
        }
        /* deal with %-N+.-M+ -- syntax is already checked */
        N = M = 0;   /* value */
        Np = Mp = 0; /* is there a plus? */
        Nd = Md = 0; /* is there a dash? */
        if (*map == '-') ++map, Nd = 1;
        N = *map++ - '0';
        if (*map == '+') ++map, Np = 1;
        if (*map == '.') {
            ++map;
            if (*map == '-') {
                ++map, Md = 1;
            }
            M = *map++ - '0';
            if (*map == '+') {
                ++map, Mp = 1;
            }
        }
        /* note that N and M are one-based indices, not zero-based */
        start = dots[0]+1; /* ptr to the first character */
        end = dots[ndots]; /* ptr to the character after the last one */
        if (N != 0) {
            if (N > ndots) {
                start = "_";
                end = start+1;
            } else if (!Nd) {
                start = dots[N-1]+1;
                if (!Np) {
                    end = dots[N];
                }
            } else {
                if (!Np) {
                    start = dots[ndots-N]+1;
                }
                end = dots[ndots-N+1];
            }
        }
        if (M != 0) {
            if (M > end - start) {
                start = "_";
                end = start+1;
            } else if (!Md) {
                start = start+M-1;
                if (!Mp) {
                    end = start+1;
                }
            } else {
                if (!Mp) {
                    start = end-M;
                }
                end = end-M+1;
            }
        }
        virtualroot_checkspace(r, buf, &dest, end - start, &docroot);
        for (p = start; p < end; ++p) {
            *dest++ = apr_tolower(*p);
        }
    }
    *dest = '\0';

    if (   apr_stat(&sb, buf, APR_FINFO_MIN, r->pool) != APR_SUCCESS
           || sb.filetype != APR_DIR) {
        return NULL;
    }

    if (docroot) {
        docroot = apr_pstrcat(r->pool, docroot, buf, NULL);
    } else {
        docroot = apr_pstrcat(r->pool, buf, NULL);
    }

    return docroot;
}

/* in order to ensure that s->document_root doesn't get corrupted by
 * modperl users setting its value, restore the original value at the
 * end of each request */
struct mvr_docroot_info {
    const char **docroot;
    const char *original;
};

static apr_status_t restore_docroot(void *data)
{
    struct mvr_docroot_info *di = (struct mvr_docroot_info *)data;
    *di->docroot  = di->original;
    return APR_SUCCESS;
}

static const char *mvr_set_document_root(request_rec *r, const char *document_root)
{
    struct mvr_docroot_info *di;
    core_server_config *conf;
    
    conf = ap_get_module_config(r->server->module_config,
                                &core_module);
    di = apr_palloc(r->pool, sizeof *di);
    di->docroot = &conf->ap_document_root;
    di->original = conf->ap_document_root;
    apr_pool_cleanup_register(r->pool, di, restore_docroot,
                              restore_docroot);

    conf->ap_document_root = apr_pstrdup(r->pool, document_root);
}

static const char *mvr_get_document_root(request_rec *r) {
    mvr_sconf_t *conf;
    const char *name, *map, *root, *domain;
    char *checkdomain;
    int i, iName, iDomain;
    char t;

    conf = (mvr_sconf_t *) ap_get_module_config(r->server->module_config,
                                                &virtualroot_module);

    root = NULL;
    map = conf->doc_root;
    if (map == NULL) {
        return NULL;
    }

    name = ap_get_server_name(r);

    if (conf->domains->nelts > 0) {
        iName = strlen(name);
        for (i = 0; i < conf->domains->nelts; i++) {
            domain = ((const char**)conf->domains->elts)[i];
            if (*domain != '.' || *(domain+1) != '\0') {
                iDomain = strlen(domain);
                checkdomain = (char *)name + (iName - iDomain);
                if (strncmp(checkdomain, domain, iDomain) != 0) {
                    continue;
                }
                t = *checkdomain;
                *checkdomain = '\0';
                root = virtualroot_interpolate(r, name, map);
                *checkdomain = t;
                
                if (root != NULL) {
                    apr_table_set(r->notes, "VIRTUALROOT_DOMAIN",
                        apr_pstrdup(r->pool, domain));
#ifndef USE_ITHREADS
                    apr_table_set(r->subprocess_env, "VIRTUALROOT_DOMAIN",
                        apr_pstrdup(r->pool, domain));
#endif
                    break;
                }
            } else {
                root = virtualroot_interpolate(r, name, map);
                if (root != NULL) {
                    break;
                }
            }
        }
    } else {
        root = virtualroot_interpolate(r, name, map);
    }
    return root;
}

#ifdef USE_ITHREADS
static int mvr_translate_name(request_rec *r)
{
    const char *root;

    if (r->uri[0] != '/') {
        return DECLINED;
    }

    root = mvr_get_document_root(r);
    if (root == NULL) {
        return DECLINED;
    }

    r->filename = apr_pstrcat(r->pool, root, r->uri, NULL);
    r->canonical_filename = "";

    return OK;
}
#else
static int mvr_post_read_request(request_rec *r)
{
    const char *root;

    root = mvr_get_document_root(r);
    if (root == NULL) {
        return OK;
    }

    mvr_set_document_root(r, root);

    return OK;
}
#endif

static void register_hooks(apr_pool_t *p)
{
#ifdef USE_ITHREADS
    static const char * const aszPre[]={ "mod_alias.c","mod_userdir.c",NULL };
    ap_hook_translate_name(mvr_translate_name, aszPre, NULL, APR_HOOK_MIDDLE);
#else
    ap_hook_post_read_request(mvr_post_read_request,
                              NULL, NULL, APR_HOOK_REALLY_FIRST);
#endif
}

module AP_MODULE_DECLARE_DATA virtualroot_module =
{
    STANDARD20_MODULE_STUFF,
    NULL,                       /* dir config creater */
    NULL,                       /* dir merger --- default is to override */
    mvr_create_server_config,   /* server config */
    mvr_merge_server_config,    /* merge server configs */
    mvr_commands,               /* command apr_table_t */
    register_hooks              /* register hooks */
};

