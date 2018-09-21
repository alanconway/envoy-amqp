# AMQP-HTTP Bridge using Envoy and Qpid Proton

**WORK IN PROGRESS** - this is not yet complete or supported.

## Overview

Goal: Extend the [Envoy][] HTTP router to translate between HTTP and the [AMQP][]
messaging protocol using the [Qpid Proton C++][] AMQP library. Use 
[Envoy][] and the [Qpid Dispatch][] AMQP router together to:

* Tunnel HTTP request/response traffic over an AMQP messaging network
* Route HTTP clients to AMQP request/response services
* Route AMQP request/response clients to HTTP services

Note: HTTP is a request-response protocol. AMQP allows other messaging patterns,
but only the AMQP request/response pattern is in scope here.

[Envoy]: https://github.com/envoyproxy
[AMQP]: https://www.amqp.org/
[Qpid Dispatch]: http://qpid.apache.org/components/dispatch-router
[Qpid Proton C++]: http://qpid.apache.org/releases/qpid-proton-0.23.0/proton/cpp/api/index.html

## Use cases

The overall use-case is an application that mixes AMQP and HTTP.

This could happen because:

1. An AMQP network application includes components that have REST APIs
   for management/monitoring, or has HTTP endpoints at the edges.
2. HTTP application uses AMQP for performance/quality of service in
   specific cases, or has AMQP at the edges.
3. A HTTP application and an AMQP application walk into a bar...

### Tunneling HTTP over AMQP

HTTP clients talking to HTTP services but routed over an AMQP
interconnect network.  HTTP URLs become AMQP addresses so they can be
configured and routed in the same way as "real" AMQP address. The AMQP
network provides a consistent point of configuration for routing,
discovery, fail-over of both HTTP and AMQP services.

### AMQP client HTTP service

AMQP clients interact with a mix of AMQP and HTTP services, but treat
them all as AMQP services.

* No need for bilingual clients or developers
* Service implementations can change without updating clients.

### HTTP client to AMQP service

HTTP clients interact with a mix of AMQP and HTTP services, but treat
them all as HTTP services.

* No need for bilingual clients or developers
* Service implementations can change without updating clients.

There are 3 sub-cases

* HTTP request/respnses to AMQP request/response server (e.g. AMQP management service)
* HTTP send a message to an AMQP server (not yet implemented)
* HTTP receive a message from an AMQP server (not yet implemented)

## Design

A pair of Envoy `Network::Filter` classes convert between AMQP messages (body
and application-properties) and HTTP messages (body and headers), and preserve
request/response correlation.

The **amqp_server** filter is configured on an Envoy listener to make it act
like an AMQP server. AMQP client requests are translated to HTTP and forwarded
to Envoy's HTTP connection manager, exactly as if they had come from an HTTP
client. HTTP responses are translated and correlated back to AMQP using the
standard `correlation_id` and `reply_to` properties.

The **amqp_client** filter is configured on an Envoy cluster (upstream
connection pool) to make it's connections act as AMQP client
connections. Outbound HTTP requests are translated to AMQP, the AMQP responses
are correlated and returned as HTTP responses.

The AMQP filters focus only on bridging between HTTP and AMQP, and managing AMQP
links. Envoy's built-in HTTP router provides rich routing and manipulation of
HTTP requests and responses, all of which can be used with bridged AMQP messages.

### Security

Envoy has full https support for HTTP client/server connections, there is no
change there.  AMQP client/server connections are subject to AMQP security
implemented by proton, which includes TLS and SASL. The two security realms are
configured separately and do not interact.

*TODO implement security configuration for filters: should it follow the proton config model or mimic/reuse Envoy tls_context?"*

*TODO investigate sharing certificates etc. between proton and Envoy*

### Combining with Qpid Dispatch

Tunneling HTTP over an AMQP network using Qpid Dispatch router looks like this:

    -HTTP-> Envoy[amqp_client] -AMQP-> {Dispatch network} -AMQP-> [amqp_server]Envoy -HTTP->

## Mapping

### Message Body

An AMQP message body [consists of one of the following three choices: one or more data sections, one or more amqp-sequence sections, or a single amqp-value section.][message-format]

A HTTP message body corresponds to an AMQP message with a single `data`
section. The HTTP Content-Type a nd Content-Encoding headers corresponds to
equivalent AMQP message properties.

If an AMQP message has multiple data sections, only the first is used, the rest
are ignored.

An AMQP messsage with a `value` section is allowed if the value has one of the
following types:

* string: HTTP Content-Type defaults to `text/plain; charset=utf-8`
* binary: HTTP Content-Type defaults to `application/octet-stream`

If the AMQP content-type is set it will be used in favour of the defaults above.

Requests with other AMQP body types (sequence sections or value sections other than the types above) will rejected via the AMQP message disposition.

[message-props]: http://docs.oasis-open.org/amqp/core/v1.0/os/amqp-core-messaging-v1.0-os.html#type-properties
[message-format]: http://docs.oasis-open.org/amqp/core/v1.0/os/amqp-core-messaging-v1.0-os.html#section-message-format

### Headers and Application-Properties

HTTP headers correspond to AMQP application-properties with string values, AMQP
application-properties with non-string values are ignored.

HTTP header names are *case-insensitive*, AMQP application-property keys are not.

To ensure consistent mapping, header names in AMQP property keys are *always
lower-case*. HTTP headers are normalized to lower-case on conversion to AMQP, and
AMQP property-keys containing capitals are ignored when converting to HTTP.

### AMQP Request to HTTP

The AMQP request:
* must have a `reply_to` address for the AMQP response.
* may have a `correlation_id`, if so it will be copied to the AMQP response.

The HTTP request line is formed from an AMQP message as follows:
* Method = `subject`
* URI = `to` message property if set, link `target` address if not.

The HTTP [Host][host-header] header is set from the URI if it has a host part,
or the AMQP virtual host if not.

[host-header]: https://www.w3.org/Protocols/rfc2616/rfc2616-sec14.html#sec14.23

### AMQP Response from HTTP

AMQP message properties:
* `subject` = HTTP status code and reason e.g. "200 OK", "400 Bad Request"
* `to` = `reply-to` address from the original AMQP request
* `correlation_id` = `correlation_id` from the original AMQP request if present

### HTTP request to AMQP

AMQP message properties:
* `subject` = HTTP Method, e.g. "GET", "POST"
* `to` = HTTP URI (with authority or path only)
* `correlation_id` = generated by the bridge
* `reply_to` = generated by the bridge

### HTTP response from AMQP

HTTP status-code is taken from AMQP `subject`

* if subject starts with a valid HTTP response code, use it.
* else use "200"

If an AMQP request is released, rejected or modified, the outcome becomes a HTTP
response "502 Bad Gateway" with body string "released", "rejected" or "modified"

## AMQP Links and Connections

Both client and server can be configured to send requests/responses via the AMQP
ANONYMOUS-RELAY (if available) and/or a named relay address.

If not configured to relay, the client will automatically create sending links
with the request `to` address as target.

The server accepts named links and dynamic-source links for use as reply-to
addresses, and offers ANONYMOUS-RELAY capability for incoming requests.
The server can also automatically create sending links if configured to do so.

Both client and server can be configured to create a fixed set of named sending
and receiving links; to advertise addresses, set up named relays etc.

## Building and testing

You need all the requirements to build Envoy, see https://www.envoyproxy.io

You need to build proton manually *TODO add bazel genrule*

    cd proton/bld && cmake -DCMAKE_INSTALL_PREFIX=install -DBUILD_STATIC_LIBS=YES -DSSL_IMPL=none .. && make install

Build the AMQP filters statically linked into envoy with:

    bazel build

There are rather rough system tests implemented in ruby, run them with:

    bazel test ...

# Things to do

## Cleanup and complete

Fixes and missing features of the bridge:

* [ ] Implement heartbeats
* [ ] Use Envoy codec for better HTTP 1.1, and HTTP 2 support.
* [ ] Error information on reject/modify dispositions
* [ ] Set AMQP host from HTTP host for multi-tenancy
* [ ] Full Proton configuration: SASL, SSL etc. (see Qpid Dispatch connector/listener config)
* [ ] Performance and stability: pipelining/multiplexing issues.
* [ ] Redirect proton logs to Envoy trace logs.

Testing

* [ ] Automated unit and integration tests under bazel
* [ ] Multi-host tests in more realistic setting (including dispatch, kubernetes etc.)
* [ ] Test all HTTP methods, not just GET and POST

Fixes to Envoy

* [ ] Submit upstream filter extensions (https://github.com/envoyproxy/envoy/issues/173)
* [ ] Callback for downstream write (https://github.com/envoyproxy/envoy/issues/33simp43)
* [ ] Load shared modules (https://github.com/envoyproxy/envoy/issues/2053)

## Missing features

*inverted server*: put an amqp_server bridge on an *outgoing* envoy connection to a router. Envoy initiates the connection but receives requests from router. Allow ephemeral Envoy side-cars to "announce" themselves to well-knonw routers.

*send/receive*: Allow HTTP clients to do simple send/receive without request-response

*service advertisement*: automatically reflect Envoy service configuration/discovery as AMQP links.

*improved flow control*: Review & limit internal buffering using AMQP credit and Envoy
watermark buffers.

## Possible optimizations

*Eliminate double HTTP codec*: amqp_bridge composes/parses HTTP by itself and exchanges HTTP bytes with the Envoy connection-manager, which must parse/compose again. Can we eliminate the double codec?

## Open questions

Default to anonymous-relay? For a server bridge, anonymous-relay is most likely the best way to send responses. For a client bridge, anonymous-relay seems a good default if available, unless a named-relay is explicitly provided. Replace anonymous-relay with a link-per-address property with the opposite meaning?
