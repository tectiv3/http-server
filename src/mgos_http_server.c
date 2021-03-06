/*
 * Copyright (c) 2014-2016 Cesanta Software Limited
 * All rights reserved
 */

#include "mgos_http_server.h"

#if defined(MGOS_HAVE_ATCA)
#include "mgos_atca.h"
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/cs_file.h"
#include "common/json_utils.h"
#include "common/str_util.h"
#include "fw/src/mgos_config.h"
#include "fw/src/mgos_debug.h"
#include "fw/src/mgos_debug_hal.h"
#include "fw/src/mgos_hal.h"
#include "fw/src/mgos_init.h"
#include "fw/src/mgos_mongoose.h"
#include "fw/src/mgos_sys_config.h"
#include "fw/src/mgos_updater_common.h"
#include "fw/src/mgos_utils.h"
#include "fw/src/mgos_wifi.h"

#define MGOS_F_RELOAD_CONFIG MG_F_USER_5

#if MG_ENABLE_FILESYSTEM
static struct mg_serve_http_opts s_http_server_opts;
#endif
static struct mg_connection *s_listen_conn;
static struct mg_connection *s_listen_conn_tun;

#if MGOS_ENABLE_WEB_CONFIG

#define JSON_HEADERS "Connection: close\r\nContent-Type: application/json"

static void send_cfg(const void *cfg, const struct mgos_conf_entry *schema,
                     struct http_message *hm, struct mg_connection *c) {
  mg_send_response_line(c, 200, JSON_HEADERS);
  mg_send(c, "\r\n", 2);
  bool pretty = (mg_vcmp(&hm->query_string, "pretty") == 0);
  mgos_conf_emit_cb(cfg, NULL, schema, pretty, &c->send_mbuf, NULL, NULL);
}

static void conf_handler(struct mg_connection *c, int ev, void *p,
                         void *user_data) {
  struct http_message *hm = (struct http_message *) p;
  if (ev != MG_EV_HTTP_REQUEST) return;
  LOG(LL_DEBUG, ("[%.*s] requested", (int) hm->uri.len, hm->uri.p));
  struct mbuf jsmb;
  struct json_out jsout = JSON_OUT_MBUF(&jsmb);
  mbuf_init(&jsmb, 0);
  char *msg = NULL;
  int status = -1;
  int rc = 200;
  if (mg_vcmp(&hm->uri, "/conf/defaults") == 0) {
    struct sys_config cfg;
    if (load_config_defaults(&cfg)) {
      send_cfg(&cfg, sys_config_schema(), hm, c);
      mgos_conf_free(sys_config_schema(), &cfg);
      status = 0;
    }
  } else if (mg_vcmp(&hm->uri, "/conf/current") == 0) {
    send_cfg(get_cfg(), sys_config_schema(), hm, c);
    status = 0;
  } else if (mg_vcmp(&hm->uri, "/conf/save") == 0) {
    struct sys_config tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (load_config_defaults(&tmp)) {
      char *acl_copy = (tmp.conf_acl == NULL ? NULL : strdup(tmp.conf_acl));
      if (mgos_conf_parse(hm->body, acl_copy, sys_config_schema(), &tmp)) {
        status = (save_cfg(&tmp, &msg) ? 0 : -10);
      } else {
        status = -11;
      }
      free(acl_copy);
    } else {
      status = -10;
    }
    mgos_conf_free(sys_config_schema(), &tmp);
    if (status == 0) c->flags |= MGOS_F_RELOAD_CONFIG;
  } else if (mg_vcmp(&hm->uri, "/conf/reset") == 0) {
    struct stat st;
    if (stat(CONF_USER_FILE, &st) == 0) {
      status = remove(CONF_USER_FILE);
    } else {
      status = 0;
    }
    if (status == 0) c->flags |= MGOS_F_RELOAD_CONFIG;
  }

  if (status != 0) {
    json_printf(&jsout, "{status: %d", status);
    if (msg != NULL) {
      json_printf(&jsout, ", message: %Q}", msg);
    } else {
      json_printf(&jsout, "}");
    }
    LOG(LL_ERROR, ("Error: %.*s", (int) jsmb.len, jsmb.buf));
    rc = 500;
  }

  if (jsmb.len > 0) {
    mg_send_head(c, rc, jsmb.len, JSON_HEADERS);
    mg_send(c, jsmb.buf, jsmb.len);
  }
  c->flags |= MG_F_SEND_AND_CLOSE;
  mbuf_free(&jsmb);
  free(msg);
  (void) user_data;
}

static void reboot_handler(struct mg_connection *c, int ev, void *p,
                           void *user_data) {
  (void) p;
  if (ev != MG_EV_HTTP_REQUEST) return;
  LOG(LL_DEBUG, ("Reboot requested"));
  mg_send_head(c, 200, 0, JSON_HEADERS);
  c->flags |= (MG_F_SEND_AND_CLOSE | MGOS_F_RELOAD_CONFIG);
  (void) user_data;
}

static void ro_vars_handler(struct mg_connection *c, int ev, void *p,
                            void *user_data) {
  if (ev != MG_EV_HTTP_REQUEST) return;
  LOG(LL_DEBUG, ("RO-vars requested"));
  struct http_message *hm = (struct http_message *) p;
  send_cfg(get_ro_vars(), sys_ro_vars_schema(), hm, c);
  c->flags |= MG_F_SEND_AND_CLOSE;
  (void) user_data;
}
#endif /* MGOS_ENABLE_WEB_CONFIG */

#if MGOS_ENABLE_FILE_UPLOAD
static struct mg_str upload_fname(struct mg_connection *nc,
                                  struct mg_str fname) {
  struct mg_str res = {NULL, 0};
  (void) nc;
  if (mgos_conf_check_access(fname, get_cfg()->http.upload_acl)) {
    res = fname;
  }
  return res;
}

static void upload_handler(struct mg_connection *c, int ev, void *p,
                           void *user_data) {
  mg_file_upload_handler(c, ev, p, upload_fname, user_data);
}
#endif

#if MGOS_ENABLE_WIFI && MGOS_ENABLE_TUNNEL
static void on_wifi_ready(enum mgos_wifi_status event, void *arg) {
  if (s_listen_conn_tun != NULL) {
    /* Depending on the WiFi status, allow or disallow tunnel reconnection */
    switch (event) {
      case MGOS_WIFI_DISCONNECTED:
        s_listen_conn_tun->flags |= MG_F_TUN_DO_NOT_RECONNECT;
        break;
      case MGOS_WIFI_IP_ACQUIRED:
        s_listen_conn_tun->flags &= ~MG_F_TUN_DO_NOT_RECONNECT;
        break;
      default:
        break;
    }
  }

  (void) arg;
}
#endif /* MGOS_ENABLE_WIFI */

static void mgos_http_ev(struct mg_connection *c, int ev, void *p,
                         void *user_data) {
  switch (ev) {
    case MG_EV_ACCEPT: {
      char addr[32];
      mg_sock_addr_to_str(&c->sa, addr, sizeof(addr),
                          MG_SOCK_STRINGIFY_IP | MG_SOCK_STRINGIFY_PORT);
      LOG(LL_INFO, ("%p HTTP connection from %s", c, addr));
      break;
    }
    case MG_EV_HTTP_REQUEST: {
#if MG_ENABLE_FILESYSTEM
      struct http_message *hm = (struct http_message *) p;
      LOG(LL_INFO, ("%p %.*s %.*s", c, (int) hm->method.len, hm->method.p,
                    (int) hm->uri.len, hm->uri.p));

      mg_serve_http(c, p, s_http_server_opts);
/*
 * NOTE: `mg_serve_http()` manages closing connection when appropriate,
 * so, we should not set `MG_F_SEND_AND_CLOSE` here
 */
#else
      mg_http_send_error(c, 404, "Not Found");
#endif
      break;
    }
    case MG_EV_HTTP_MULTIPART_REQUEST: {
      mg_http_send_error(c, 404, "Not Found");
      break;
    }
    case MG_EV_CLOSE: {
      /* If we've sent the reply to the server, and should reboot, reboot */
      if (c->flags & MGOS_F_RELOAD_CONFIG) {
        c->flags &= ~MGOS_F_RELOAD_CONFIG;
        mgos_system_restart(0);
      }
      break;
    }
  }
  (void) user_data;
}

bool mgos_http_server_init(void) {
  const struct sys_config_http *cfg = &get_cfg()->http;

  if (!cfg->enable) {
    return true;
  }

  if (cfg->listen_addr == NULL) {
    LOG(LL_WARN, ("HTTP Server disabled, listening address is empty"));
    return true; /* At this moment it is just warning */
  }

#if MG_ENABLE_FILESYSTEM
  s_http_server_opts.hidden_file_pattern = cfg->hidden_files;
  s_http_server_opts.auth_domain = cfg->auth_domain;
  s_http_server_opts.global_auth_file = cfg->auth_file;
#endif

  struct mg_bind_opts opts;
  memset(&opts, 0, sizeof(opts));
#if MG_ENABLE_SSL
  opts.ssl_cert = cfg->ssl_cert;
  opts.ssl_key = cfg->ssl_key;
  opts.ssl_ca_cert = cfg->ssl_ca_cert;
#if CS_PLATFORM == CS_P_ESP8266
/*
 * ESP8266 cannot handle DH of any kind, unless there's hardware acceleration,
 * it's too slow.
 */
#if defined(MGOS_HAVE_ATCA)
  if (mbedtls_atca_is_available()) {
    opts.ssl_cipher_suites =
        "TLS-ECDHE-ECDSA-WITH-AES-128-GCM-SHA256:"
        "TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256:"
        "TLS-ECDH-ECDSA-WITH-AES-128-GCM-SHA256:"
        "TLS-ECDH-ECDSA-WITH-AES-128-CBC-SHA256:"
        "TLS-ECDH-ECDSA-WITH-AES-128-CBC-SHA:"
        "TLS-ECDH-RSA-WITH-AES-128-GCM-SHA256:"
        "TLS-ECDH-RSA-WITH-AES-128-CBC-SHA256:"
        "TLS-ECDH-RSA-WITH-AES-128-CBC-SHA:"
        "TLS-RSA-WITH-AES-128-GCM-SHA256:"
        "TLS-RSA-WITH-AES-128-CBC-SHA256:"
        "TLS-RSA-WITH-AES-128-CBC-SHA";
  } else
#endif /* defined(MGOS_HAVE_ATCA) */
    opts.ssl_cipher_suites =
        "TLS-RSA-WITH-AES-128-GCM-SHA256:"
        "TLS-RSA-WITH-AES-128-CBC-SHA256:"
        "TLS-RSA-WITH-AES-128-CBC-SHA";
#endif /* CS_PLATFORM == CS_P_ESP8266 */
#endif /* MG_ENABLE_SSL */
  s_listen_conn =
      mg_bind_opt(mgos_get_mgr(), cfg->listen_addr, mgos_http_ev, NULL, opts);

  if (!s_listen_conn) {
    LOG(LL_ERROR, ("Error binding to [%s]", cfg->listen_addr));
    return false;
  }

  mg_set_protocol_http_websocket(s_listen_conn);
  LOG(LL_INFO, ("HTTP server started on [%s]%s", cfg->listen_addr,
#if MG_ENABLE_SSL
                (opts.ssl_cert ? " (SSL)" : "")
#else
                ""
#endif
                    ));

#if MGOS_ENABLE_TUNNEL
  const struct sys_config_device *device_cfg = &get_cfg()->device;
  if (cfg->tunnel.enable && device_cfg->id != NULL &&
      device_cfg->password != NULL) {
    char *tun_addr = NULL;
    /*
     * NOTE: we won't free `tun_addr`, because when reconnect happens, this
     * address string will be accessed again.
     */
    if (mg_asprintf(&tun_addr, 0, "ws://%s:%s@%s.%s", device_cfg->id,
                    device_cfg->password, device_cfg->id,
                    cfg->tunnel.addr) < 0) {
      return false;
    }
    s_listen_conn_tun =
        mg_bind_opt(mgos_get_mgr(), tun_addr, mgos_http_ev, opts);

    if (s_listen_conn_tun == NULL) {
      LOG(LL_ERROR, ("Error binding to [%s]", tun_addr));
      return false;
    } else {
#if MGOS_ENABLE_WIFI
      /*
       * Wifi is not yet ready, so we need to set a flag which prevents the
       * tunnel from reconnecting. The flag will be cleared when wifi connection
       * is ready.
       */
      s_listen_conn_tun->flags |= MG_F_TUN_DO_NOT_RECONNECT;
      mgos_wifi_add_on_change_cb(on_wifi_ready, NULL);
#endif
    }

    mg_set_protocol_http_websocket(s_listen_conn_tun);
    LOG(LL_INFO, ("Tunneled HTTP server started on [%s]%s", tun_addr,
#if MG_ENABLE_SSL
                  (opts.ssl_cert ? " (SSL)" : "")
#else
                  ""
#endif
                      ));
  }
#endif

#if MGOS_ENABLE_WEB_CONFIG
  mgos_register_http_endpoint("/conf/", conf_handler, NULL);
  mgos_register_http_endpoint("/reboot", reboot_handler, NULL);
  mgos_register_http_endpoint("/ro_vars", ro_vars_handler, NULL);
#endif
#if MGOS_ENABLE_FILE_UPLOAD
  mgos_register_http_endpoint("/upload", upload_handler, NULL);
#endif

  return true;
}

void mgos_register_http_endpoint_opt(const char *uri_path,
                                     mg_event_handler_t handler,
                                     struct mg_http_endpoint_opts opts) {
  if (s_listen_conn != NULL) {
    mg_register_http_endpoint_opt(s_listen_conn, uri_path, handler, opts);
  }
  if (s_listen_conn_tun != NULL) {
    mg_register_http_endpoint_opt(s_listen_conn_tun, uri_path, handler, opts);
  }
}

void mgos_register_http_endpoint(const char *uri_path,
                                 mg_event_handler_t handler, void *user_data) {
  struct mg_http_endpoint_opts opts;
  memset(&opts, 0, sizeof(opts));
  opts.user_data = user_data;
  opts.auth_domain = get_cfg()->http.auth_domain;
  opts.auth_file = get_cfg()->http.auth_file;
  mgos_register_http_endpoint_opt(uri_path, handler, opts);
}

struct mg_connection *mgos_get_sys_http_server(void) {
  return s_listen_conn;
}
