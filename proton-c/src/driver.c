/*
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */

#define _POSIX_C_SOURCE 1

#include <proton/driver.h>
#include <proton/sasl.h>
#include <proton/ssl.h>
#include "util.h"
#include "driver-internal.h"
#include "ssl/ssl-internal.h"

#include <stdio.h>
#include <time.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>


/* Impls */

// listener

static void pn_driver_add_listener(pn_driver_t *d, pn_listener_t *l)
{
  if (!l->driver) return;
  LL_ADD(d, listener, l);
  l->driver = d;
  d->listener_count++;
}

static void pn_driver_remove_listener(pn_driver_t *d, pn_listener_t *l)
{
  if (!l->driver) return;

  if (l == d->listener_next) {
    d->listener_next = l->listener_next;
  }

  LL_REMOVE(d, listener, l);
  l->driver = NULL;
  d->listener_count--;
}

pn_listener_t *pn_listener(pn_driver_t *driver, const char *host,
                           const char *port, void* context)
{
  if (!driver) return NULL;

  struct addrinfo *addr;
  int code = getaddrinfo(host, port, NULL, &addr);
  if (code) {
    pn_error_format(driver->error, PN_ERR, "getaddrinfo: %s\n", gai_strerror(code));
    return NULL;
  }

  int sock = socket(AF_INET, SOCK_STREAM, getprotobyname("tcp")->p_proto);
  if (sock == -1) {
    pn_error_from_errno(driver->error, "socket");
    return NULL;
  }

  int optval = 1;
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
    pn_error_from_errno(driver->error, "setsockopt");
    return NULL;
  }

  if (bind(sock, addr->ai_addr, addr->ai_addrlen) == -1) {
    pn_error_from_errno(driver->error, "bind");
    freeaddrinfo(addr);
    return NULL;
  }

  freeaddrinfo(addr);

  if (listen(sock, 50) == -1) {
    pn_error_from_errno(driver->error, "listen");
    return NULL;
  }

  pn_listener_t *l = pn_listener_fd(driver, sock, context);

  if (driver->trace & (PN_TRACE_FRM | PN_TRACE_RAW | PN_TRACE_DRV))
    fprintf(stderr, "Listening on %s:%s\n", host, port);
  return l;
}

pn_listener_t *pn_listener_fd(pn_driver_t *driver, int fd, void *context)
{
  if (!driver) return NULL;

  pn_listener_t *l = malloc(sizeof(pn_listener_t));
  if (!l) return NULL;
  l->driver = driver;
  l->listener_next = NULL;
  l->listener_prev = NULL;
  l->pending = false;
  l->fd = fd;
  l->context = context;

  pn_listener_poller_init(l);

  pn_driver_add_listener(driver, l);
  return l;
}

pn_listener_t *pn_listener_head(pn_driver_t *driver)
{
  return driver ? driver->listener_head : NULL;
}

pn_listener_t *pn_listener_next(pn_listener_t *listener)
{
  return listener ? listener->listener_next : NULL;
}

void pn_listener_trace(pn_listener_t *l, pn_trace_t trace) {
  // XXX
}

void *pn_listener_context(pn_listener_t *l) {
  return l ? l->context : NULL;
}

static void pn_configure_sock(int sock) {
  // this would be nice, but doesn't appear to exist on linux
  /*
  int set = 1;
  if (!setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, (void *)&set, sizeof(int))) {
    perror("setsockopt");
  };
  */
}

pn_connector_t *pn_listener_accept(pn_listener_t *l)
{
  if (!l || !l->pending) return NULL;

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  socklen_t addrlen = sizeof(addr);
  int sock = accept(l->fd, (struct sockaddr *) &addr, &addrlen);
  if (sock == -1) {
    perror("accept");
    return NULL;
  } else {
    char host[1024], serv[64];
    int code;
    if ((code = getnameinfo((struct sockaddr *) &addr, addrlen, host, 1024, serv, 64, 0))) {
      fprintf(stderr, "getnameinfo: %s\n", gai_strerror(code));
      if (close(sock) == -1)
        perror("close");
      return NULL;
    } else {
      pn_configure_sock(sock);
      if (l->driver->trace & (PN_TRACE_FRM | PN_TRACE_RAW | PN_TRACE_DRV))
        fprintf(stderr, "Accepted from %s:%s\n", host, serv);
      pn_connector_t *c = pn_connector_fd(l->driver, sock, NULL);
      snprintf(c->name, PN_CONNECTOR_NAME_MAX, "%s:%s", host, serv);
      c->listener = l;
      return c;
    }
  }
}

void pn_listener_close(pn_listener_t *l)
{
  if (!l) return;

  if (close(l->fd) == -1)
    perror("close");
}

void pn_listener_free(pn_listener_t *l)
{
  if (!l) return;

  if (l->driver) pn_driver_remove_listener(l->driver, l);
  pn_listener_poller_destroy(l);
  free(l);
}

// connector

static void pn_driver_add_connector(pn_driver_t *d, pn_connector_t *c)
{
  if (!c->driver) return;
  LL_ADD(d, connector, c);
  c->driver = d;
  d->connector_count++;
}

static void pn_driver_remove_connector(pn_driver_t *d, pn_connector_t *c)
{
  if (!c->driver) return;

  if (c == d->connector_next) {
    d->connector_next = c->connector_next;
  }

  LL_REMOVE(d, connector, c);
  c->driver = NULL;
  d->connector_count--;
  if (c->closed) {
    d->closed_count--;
  }
}

pn_connector_t *pn_connector(pn_driver_t *driver, const char *host,
                             const char *port, void *context)
{
  if (!driver) return NULL;

  struct addrinfo *addr;
  int code = getaddrinfo(host, port, NULL, &addr);
  if (code) {
    pn_error_format(driver->error, PN_ERR, "getaddrinfo: %s", gai_strerror(code));
    return NULL;
  }

  int sock = socket(AF_INET, SOCK_STREAM, getprotobyname("tcp")->p_proto);
  if (sock == -1) {
    pn_error_from_errno(driver->error, "socket");
    return NULL;
  }

  pn_configure_sock(sock);

  if (connect(sock, addr->ai_addr, addr->ai_addrlen) == -1) {
    pn_error_from_errno(driver->error, "connect");
    freeaddrinfo(addr);
    return NULL;
  }

  freeaddrinfo(addr);

  pn_connector_t *c = pn_connector_fd(driver, sock, context);
  snprintf(c->name, PN_CONNECTOR_NAME_MAX, "%s:%s", host, port);
  if (driver->trace & (PN_TRACE_FRM | PN_TRACE_RAW | PN_TRACE_DRV))
    fprintf(stderr, "Connected to %s\n", c->name);
  return c;
}

static void pn_connector_read(pn_connector_t *ctor);
static void pn_connector_write(pn_connector_t *ctor);
static time_t pn_connector_tick(pn_connector_t *ctor, time_t now);

pn_connector_t *pn_connector_fd(pn_driver_t *driver, int fd, void *context)
{
  if (!driver) return NULL;

  pn_connector_t *c = malloc(sizeof(pn_connector_t));
  if (!c) return NULL;
  c->driver = driver;
  c->connector_next = NULL;
  c->connector_prev = NULL;
  c->pending_tick = false;
  c->pending_read = false;
  c->pending_write = false;
  c->name[0] = '\0';
  c->fd = fd;
  c->status = PN_SEL_RD | PN_SEL_WR;
  c->trace = driver->trace;
  c->closed = false;
  c->wakeup = 0;
  c->read = pn_connector_read;
  c->write = pn_connector_write;
  c->tick = pn_connector_tick;
  c->io_handler = pn_io_handler;
  c->input_size = 0;
  c->input_eos = false;
  c->output_size = 0;
  c->connection = NULL;
  c->transport = pn_transport();
  c->sasl = pn_sasl(c->transport);
  c->input_done = false;
  c->output_done = false;
  c->context = context;
  c->listener = NULL;

  pn_connector_poller_init(c);

  pn_connector_trace(c, driver->trace);

  pn_driver_add_connector(driver, c);
  return c;
}

pn_connector_t *pn_connector_head(pn_driver_t *driver)
{
  return driver ? driver->connector_head : NULL;
}

pn_connector_t *pn_connector_next(pn_connector_t *connector)
{
  return connector ? connector->connector_next : NULL;
}

void pn_connector_trace(pn_connector_t *ctor, pn_trace_t trace)
{
  if (!ctor) return;
  ctor->trace = trace;
  if (ctor->transport) pn_trace(ctor->transport, trace);
}

pn_sasl_t *pn_connector_sasl(pn_connector_t *ctor)
{
  return ctor ? ctor->sasl : NULL;
}

pn_ssl_t *pn_connector_ssl(pn_connector_t *ctor)
{
  if (ctor) {
    // use server mode SSL if this connector was created by a listener
    if (ctor->listener)
      return pn_ssl_server(ctor->transport);
    return pn_ssl_client(ctor->transport);
  }
  return NULL;
}

void pn_connector_set_connection(pn_connector_t *ctor, pn_connection_t *connection)
{
  if (!ctor) return;
  ctor->connection = connection;
  pn_transport_bind(ctor->transport, connection);
  if (ctor->transport) pn_trace(ctor->transport, ctor->trace);
}

pn_connection_t *pn_connector_connection(pn_connector_t *ctor)
{
  return ctor ? ctor->connection : NULL;
}

void *pn_connector_context(pn_connector_t *ctor)
{
  return ctor ? ctor->context : NULL;
}

void pn_connector_set_context(pn_connector_t *ctor, void *context)
{
  if (!ctor) return;
  ctor->context = context;
}

pn_listener_t *pn_connector_listener(pn_connector_t *ctor)
{
  return ctor ? ctor->listener : NULL;
}

void pn_connector_close(pn_connector_t *ctor)
{
  // XXX: should probably signal engine and callback here
  if (!ctor) return;

  ctor->status = 0;
  if (close(ctor->fd) == -1)
    perror("close");
  ctor->closed = true;
  ctor->driver->closed_count++;
}

bool pn_connector_closed(pn_connector_t *ctor)
{
  return ctor ? ctor->closed : true;
}

void pn_connector_free(pn_connector_t *ctor)
{
  if (!ctor) return;

  if (ctor->driver) pn_driver_remove_connector(ctor->driver, ctor);

  pn_connector_poller_destroy(ctor);
  ctor->connection = NULL;
  pn_transport_free(ctor->transport);
  ctor->transport = NULL;
  free(ctor);
}

static void pn_connector_read(pn_connector_t *ctor)
{
  ssize_t n = recv(ctor->fd, ctor->input + ctor->input_size, PN_CONNECTOR_IO_BUF_SIZE - ctor->input_size, 0);
  if (n <= 0) {
    if (n < 0) perror("read");
    ctor->status &= ~PN_SEL_RD;
    ctor->input_eos = true;
  } else {
    ctor->input_size += n;
  }
}

static void pn_connector_consume(pn_connector_t *ctor, int n)
{
  ctor->input_size -= n;
  memmove(ctor->input, ctor->input + n, ctor->input_size);
}

void pn_connector_process_input(pn_connector_t *ctor)
{
  pn_transport_t *transport = ctor->transport;
  if (!ctor->input_done) {
    if (ctor->input_size > 0 || ctor->input_eos) {
      ssize_t n = pn_input(transport, ctor->input, ctor->input_size);
      if (n >= 0) {
        pn_connector_consume(ctor, n);
      } else {
        pn_connector_consume(ctor, ctor->input_size);
        ctor->input_done = true;
      }
    }
  }
}

static char *pn_connector_output(pn_connector_t *ctor)
{
  return ctor->output + ctor->output_size;
}

static size_t pn_connector_available(pn_connector_t *ctor)
{
  return PN_CONNECTOR_IO_BUF_SIZE - ctor->output_size;
}

void pn_connector_process_output(pn_connector_t *ctor)
{
  pn_transport_t *transport = ctor->transport;
  if (!ctor->output_done) {
    ssize_t n = pn_output(transport, pn_connector_output(ctor), pn_connector_available(ctor));
    if (n >= 0) {
      ctor->output_size += n;
    } else {
      ctor->output_done = true;
    }
  }

  if (ctor->output_size) {
    ctor->status |= PN_SEL_WR;
  }
}

static void pn_connector_write(pn_connector_t *ctor)
{
  if (ctor->output_size > 0) {
    ssize_t n = send(ctor->fd, ctor->output, ctor->output_size, MSG_NOSIGNAL);
    if (n < 0) {
      // XXX
      perror("send");
      ctor->output_size = 0;
      ctor->output_done = true;
    } else {
      ctor->output_size -= n;
      memmove(ctor->output, ctor->output + n, ctor->output_size);
    }
  }

  if (!ctor->output_size)
    ctor->status &= ~PN_SEL_WR;
}

static time_t pn_connector_tick(pn_connector_t *ctor, time_t now)
{
  if (!ctor->transport) return 0;
  // XXX: should probably have a function pointer for this and switch it with different layers
  time_t result = pn_tick(ctor->transport, now);
  pn_connector_process_input(ctor);
  pn_connector_process_output(ctor);
  return result;
}

void pn_connector_process(pn_connector_t *c) {
  if (c) {
    if (c->closed) return;

    if (c->pending_tick) {
      // XXX: should handle timing also
      c->tick(c, 0);
      c->pending_tick = false;
    }

    int rc;
    rc = c->io_handler(c);
    if (rc) {
        fprintf(stderr, "I/O Failure: %d\n", rc);
        pn_connector_close(c);
        return;
    }

    if (c->output_size == 0 && c->input_done && c->output_done) {
      if (c->trace & (PN_TRACE_FRM | PN_TRACE_RAW | PN_TRACE_DRV)) {
        fprintf(stderr, "Closed %s\n", c->name);
      }
      pn_connector_shutdown_ssl(c);   // AMQP finished, perform clean shutdown
    }
  }
}

// driver

pn_driver_t *pn_driver()
{
  pn_driver_t *d = malloc(sizeof(pn_driver_t));
  if (!d) return NULL;
  d->error = pn_error();
  d->listener_head = NULL;
  d->listener_tail = NULL;
  d->listener_next = NULL;
  d->connector_head = NULL;
  d->connector_tail = NULL;
  d->connector_next = NULL;
  d->listener_count = 0;
  d->connector_count = 0;
  d->closed_count = 0;
  d->ctrl[0] = 0;
  d->ctrl[1] = 0;
  d->trace = ((pn_env_bool("PN_TRACE_RAW") ? PN_TRACE_RAW : PN_TRACE_OFF) |
              (pn_env_bool("PN_TRACE_FRM") ? PN_TRACE_FRM : PN_TRACE_OFF) |
              (pn_env_bool("PN_TRACE_DRV") ? PN_TRACE_DRV : PN_TRACE_OFF));

  // XXX
  if (pipe(d->ctrl)) {
    perror("Can't create control pipe");
  }

  pn_driver_poller_init(d);

  return d;
}

int pn_driver_errno(pn_driver_t *d)
{
  return d ? pn_error_code(d->error) : PN_ARG_ERR;
}

const char *pn_driver_error(pn_driver_t *d)
{
  return d ? pn_error_text(d->error) : NULL;
}

void pn_driver_trace(pn_driver_t *d, pn_trace_t trace)
{
  d->trace = trace;
}

void pn_driver_free(pn_driver_t *d)
{
  if (!d) return;

  close(d->ctrl[0]);
  close(d->ctrl[1]);
  while (d->connector_head)
    pn_connector_free(d->connector_head);
  while (d->listener_head)
    pn_listener_free(d->listener_head);

  pn_driver_poller_destroy(d);
  pn_error_free(d->error);
  free(d);
}

void pn_driver_wakeup(pn_driver_t *d)
{
  if (d) {
    write(d->ctrl[1], "x", 1);
  }
}


void pn_driver_wait(pn_driver_t *d, int timeout)
{
  // if SSL/TlS has data available, no need to wait for I/O
  if (!pn_driver_ssl_data_ready(d)) {
      pn_driver_poller_wait(d, timeout);
  }
  d->listener_next = d->listener_head;
  d->connector_next = d->connector_head;
}

pn_listener_t *pn_driver_listener(pn_driver_t *d) {
  if (!d) return NULL;

  while (d->listener_next) {
    pn_listener_t *l = d->listener_next;
    d->listener_next = l->listener_next;

    if (l->pending) {
      return l;
    }
  }

  return NULL;
}

pn_connector_t *pn_driver_connector(pn_driver_t *d) {
  if (!d) return NULL;

  while (d->connector_next) {
    pn_connector_t *c = d->connector_next;
    d->connector_next = c->connector_next;

    if (c->closed || c->pending_read || c->pending_write || c->pending_tick ||
        c->input_size || c->input_eos) {
      return c;
    }
  }

  return NULL;
}


/** default I/O handling routine */
int pn_io_handler(pn_connector_t *c)
{
    if (c->pending_read) {
      c->read(c);
      c->pending_read = false;
    }
    pn_connector_process_input(c);
    pn_connector_process_output(c);
    if (c->pending_write) {
      c->write(c);
      c->pending_write = false;
    }
    return 0;
}

/** stub handling routine */
int pn_null_io_handler(pn_connector_t *c)
{
    return 0;
}


