#ifndef PROTON_SRC_DRIVERS_SSL_H
#define PROTON_SRC_DRIVERS_SSL_H 1
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

/** @file
 * Internal API for SSL/TLS support in the Driver Layer.
 *
 * Generic API to abstract the implementation of SSL/TLS from the Driver codebase.
 *
 */

/** Get the SSL server object associated with a transport.
 *
 * A transport may be configured to be either an SSL server or an SSL client.  Those
 * transports that will be used to accept incoming connections must be configured as an
 * SSL server.
 *
 * @return a pointer to the SSL server object configured for this transport.  Returns NULL
 * if the server cannot be configured, which would occur if no SSL support is available,
 * or the transport has already been configured as an SSL client.
 */
pn_ssl_t *pn_ssl_server(pn_transport_t *transport);

/** Get the SSL client object associated with a transport.
 *
 * A transport may be configured to be either an SSL server or an SSL client.  Those
 * transports that will be used to initiate outbound connections must be configured as an
 * SSL client.
 *
 * @return a pointer to the SSL client object configured for this transport.  Returns NULL
 * if the client cannot be configured, which would occur if no SSL support is available,
 * or the transport has already been configured as an SSL server.
 */
pn_ssl_t *pn_ssl_client(pn_transport_t *transport);

/** Start the SSL/TLS shutdown handshake.
 *
 * The SSL/TLS shutdown involves a protocol handshake.  This call will initiate the
 * shutdown process, which may not complete on return from this function.  Once the
 * handshake is completed, the connector will be closed and pn_connector_closed() will
 * return TRUE.
 *
 * @param[in,out] c the connector to shutdown.
 */
void pn_ssl_shutdown( pn_ssl_t *ssl);

/** Release any SSL/TLS related resources used by the listener.
 *
 * @param[in,out] l the listener to clean up.
 */
void pn_ssl_free( pn_ssl_t *ssl);

/** Check if the SSL/TLS layer has data ready for reading or writing
 *
 * @param[in] d the driver
 * @return 0 if no data ready, else !0
 */
int pn_driver_ssl_data_ready( pn_driver_t *d );

void pn_connector_shutdown_ssl(pn_connector_t *c);   // fuck you
#endif /* ssl.h */
