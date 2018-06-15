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

## Implementation

Envoy `Network::Filter` classes convert between AMQP messages (body and
application-properties) to HTTP messages (body and headers). They also preserve
the request/response relationship: HTTP expects responses in request order; AMQP
uses a correlation-id to match out-of-order responses to requests.

Currently there are 2 filters:

- `amqp_server`: configured on an Envoy `listener` to make it act as an AMQP
  server connection. AMQP requests are translated and forwarded to the Envoy
  HTTP router, HTTP responses are translated back to correlated AMQP responses
  and sent to the original request's `reply_to` address.

- `amqp_client`: configured on an Envoy `cluster` to make it's connections act
  as AMQP client connections. Outbound HTTP requests are translated to AMQP, the
  AMQP responses are correlated and returned in order as HTTP responses.

Some parts of the AMQP protocol (links, flow control, settlement, heartbeats
etc.) are handled by the filter and not reflected in the HTTP
conversation. Likewise some HTTP non-terminal responses (redirects,
informational responses, etc.) are not reflected in AMQP.

## Mapping

<a name="message"></a>
### Message Body

An AMQP message body [consists of one of the following three choices: one or more data sections, one or more amqp-sequence sections, or a single amqp-value section.][message-format]

A HTTP message body corresponds to an AMQP message with a single `data`
section. The HTTP Content-Type and Content-Encoding headers corresponds to
equivalent AMQP message properties.

If an AMQP message has multiple data sections, only the first is used, the rest are ignored.

An AMQP messsage with a `value` section is allowed if the value has one of the following types:
- string: default to `Content-Type: text/plain; charset=utf-8`
- binary: default to `Content-Type: application/octet-stream`

Although the AMQP spec states ["When using an application-data section with a section code other than data, content-type SHOULD NOT be set"][message-props], the AMQP `content-type` will be respected if it is set.

Requests with other AMQP bodies (sequence sections, or value sections other than the types above) will receive get a "400 Bad Request" response.

[message-props]: http://docs.oasis-open.org/amqp/core/v1.0/os/amqp-core-messaging-v1.0-os.html#type-properties
[message-format]: http://docs.oasis-open.org/amqp/core/v1.0/os/amqp-core-messaging-v1.0-os.html#section-message-format

### Headers and Application-Properties

HTTP headers correspond to AMQP application-properties with string values,
AMQP application-properties with non-string values are ignored.

HTTP header names are case-insensitive, an AMQP request must not have properties
with names that differ only in case.

The following case-insensitive names are set automatically as headers by the bridge. They will be ignored if they occur as keys in the AMQP application-properties map:

- "Content-Type"
- "Content-Encoding"
- "Content-Length"
- "Host"
- "Status"

### AMQP Request to HTTP

The AMQP request:
* must have a `reply_to` address for the AMQP response.
* may have a `correlation_id`, if so it will be copied to the AMQP response.

The HTTP request line is formed as follows:
* Method = AMQP `subject`
* URI = the first of the following that is not empty:
  * AMQP `to` message property
  * Target address of the link the message arrived on
  * "/"

The HTTP [Host][host-header] header is set from the URI, if it has a host part,
or the AMQP virtual host if not.

[host-header]: https://www.w3.org/Protocols/rfc2616/rfc2616-sec14.html#sec14.23

### AMQP Response from HTTP

AMQP message properties:
* `subject` = HTTP reason-phrase string, e.g. "OK" or "Bad Request"
* `to` = `reply-to` address from the original AMQP request
* `correlation_id` = `correlation_id` from the original AMQP request if present
* `application-property["status"]` = string, e.g. "200", "404"

Non-terminal HTTP responses (1xx Information and 3xx Redirect) are handled by
the bridge, the are not visible to the AMQP side of the connection.

### HTTP request to AMQP

AMQP message properties:
* `subject` = HTTP Method
* `to` = HTTP URI
* `correlation_id` = generated by the bridge
* `reply_to` = generated by the bridge

### HTTP response from AMQP

HTTP status-code is taken from AMQP `subject`
- if subject starts with a valid HTTP response code, use it.
- else use "200"

If an AMQP request is released, rejected or modified, the outcome becomes a HTTP
response "502 Bad Gateway" with body string "released", "rejected" or "modified"

## AMQP Links and Connections

### Client

The client creates AMQP links to send requests and receive responses.

Request sender links are created to the first of the following targets that
applies:
- the `request_relay` address, if configured.
- the anonymous target, if peer offers ANONYMOUS-RELAY capability.
- the message 'to' address.

Response receiver links are created from the first of the following sources that
applies:
- the `response_link` configuration entry, if not empty.
- a dynamic source link, used for all responses.

### Server

The server offers the ANONYMOUS-RELAY capability and accepts any of the following links:
- dynamic source links to send responses with matching reply-to.
- named source links to send responses with matching reply-to.
- named or anonymous target links to receive requests.

The server can create the following links:
- receiver from `request_relay` source if configured, to subscribe for requests.
- sender to anonymous target, if peer offers ANONYMOUS-RELAY, to send responses.
- *TODO* receiver for URIs known to Envoy, to advertise service availability.

### AMQP Server Filter

The server filter accepts incoming links to any target address, including the
anonymous target, for incoming requests. HTTP requests are addressed as per the
Mapping above.

It accepts incoming *dynamic source* links to to be used when sending responses
to a reply-to address.

If the peer offers "ANONYMOUS-RELAY" capability, the server will *create* an anonymous-relay link and use it to send responses.

## Building and testing

*TODO instructions - using static proton build*
*TODO Need proper automated tests*

    bazel test ...

Runs ruby test client/server envoy with the bridge.


<a name="todo"></a>
# TODO lists

## Internal cleanup
Minor fixes and missing features of the bridge:
- [ ] FIXME and TODO comments
- [ ] Timers and heartbeats
- [ ] Configuration of hard-coded values: credit window, container-id etc.
- [ ] Flow control: use AMQP credit and/or Envoy watermark buffers to apply back-pressure
- [ ] Use Envoy's HTTP codec - maturity, support for HTTP2
Testing
- [ ] Automated unit and integration tests under bazel
- [ ] Multi-host tests in more realistic setting (including dispatch, kubernetes etc.)
- [ ] redirect proton logs to Envoy trace logs.
- [ ] test all HTTP methods, not just GET and POST

Fixes to Envoy
- [ ] Submit envoy upstream filter extensions (https://github.com/envoyproxy/envoy/issues/173)
- [ ] Network filter callback for downstream write (https://github.com/envoyproxy/envoy/issues/3343)
- [ ] Envoy to load shared modules (https://github.com/envoyproxy/envoy/issues/2053)

## Missing features

*anonymous relay*: The bridge should offer AMQP ANONYMOUS-RELAY capability, and
should use it if the peer offers it.

*service advertisement*: A bridge connected to a dispatch router should be able
to create AMQP receiver links based on envoy configuration and/or dynamic
service discover - in effect advertising those services as AMQP addresses to the
router.

*inverted connections*: A bride should be able to initiate a connection to a
dispatch router, but then treat it like a connection from a client - allowing
incoming requests. Use case is to allow Envoy side-cars to "announce" themselves
to a well known router, rather than requiring the router to connect to every
side-car. This requires some work in Envoy, which currently assumes that that
client/server roles are established by the direction of the connection.

## Possible optimizations

*Eliminate double HTTP codec*: Presently the envoy.connection_manager is the only "boundary" filter between a raw Network::Filter chain and a HTTP filter chain. The amqp-bridge filter composes/parses HTTP by itself and exchanges HTTP bytes with the connection-manager, which must parse/compose again. Ideally the amqp-bridge would exchange Envoy HTTP data structures directly with the connection manager or HTTP filter chain to eliminate the duplicated work. A similar argument applies for upstream filters. Needs some thought about best to hook these things together.

