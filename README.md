# AMQP-HTTP Bridge using Envoy and Qpid Proton

**WORK IN PROGRESS** - this is not yet complete or supported.

Extend the [Envoy] HTTP router to translate between HTTP and the [AMQP]
messaging protocol. The extension is implemented with [Qpid Proton] C++. The
goal is to get [Envoy] talking to the [Qpid Dispatch] AMQP router
so we can:

* Tunnel HTTP request/response traffic over an AMQP messaging network
* Route HTTP clients to AMQP request/response services
* Route AMQP request/response clients to HTTP services

Note: HTTP is inherently request-response. AMQP allows other messaging patterns,
but only request/response is considered here.

[Envoy]: https://github.com/envoyproxy
[AMQP]: https://www.amqp.org/
[Qpid Dispatch]: http://qpid.apache.org/components/dispatch-router
[Qpid Proton]: http://qpid.apache.org/proton/

## Architecture

An Envoy `Network::Filter` that converts between AMQP messages (body and
application-properties) to HTTP messages (body and headers) and preserves the
request/response relationship across the protocols.

Some parts of the AMQP protocol (links, flow control, settlement, heartbeats
etc.) are handled by the filter and not reflected in the HTTP
conversation. Likewise some HTTP interactions (redirects, informational
responses, etc.) are not reflected in AMQP.

*TODO maybe there are cases for forwarding redirect information? When would this make sense?*

AMQP request messages are converted to HTTP requests, and are not settled until
there is a HTTP response or connection failure.  On a success (a 2xx HTTP
response code) the AMQP request message is settled as *accepted* and the HTTP
response is converted to AMQP and sent to the *reply-to* address.

*TODO other half - AMQP out of envoy, AMQP addresses exposed as HTTP services to envoy clients*

## AMQP-HTTP Conversion

<a name="body"></a>

### Message body

[AMQP][message-format] provides several choices for encoding the message body:

1. A single data section

    This is very similar to a HTTP message body: a sequence of bytes, of known
    length, described by MIME types ([RFC2046] and [RFC2616]). The MIME
    `Content-Type` and `Content-Encoding` are used in HTTP and AMQP with the
    same meaning. In this case conversion just means copying the data and MIME
    information.

2. Sequence section(s) or multiple data sections

    There is no obvious way to translate the structure of these AMQP bodies to
    HTTP.  Instead copy the entire AMQP body verbatim to the HTTP body and use
    `Content-Type=application/amqp`. Such a HTTP message is only useful if it
    eventually gets translated back to AMQP, if so an AMQP decoder will have
    enough information to decode the body exactly.

    **Note**: `application/amqp` is not an official MIME type, it could not become one if there is use for it


3. A single value section

    This can be handled just like 2. using `Content-Type=application/amqp`

    *TODO consider special cases for common types* e.g. AMQP string => `Content-Type: text/plain`. Such conversions are not 100% reversible in an AMQP => HTTP => AMQP round-trip but they're "sort of" the same thing - given AMQP's redundant encoding options . I believe most proton clients would treat them the same way. This might be an essential interop feature or a cause of endless trouble.

[RFC2046]: https://tools.ietf.org/html/rfc2046
[RFC2616]: https://tools.ietf.org/html/rfc2046
[message-format]: http://docs.oasis-open.org/amqp/core/v1.0/os/amqp-core-messaging-v1.0-os.html#section-message-format

<a name="headers"></a>

### Headers and Properties

AMQP *application-properties* are translated to/from HTTP *headers*

The AMQP property key string corresponds to the HTTP header name.

HTTP header values become string values in the AMQP property map.

AMQP property values become HTTP header value strings as follows:
- string values are unchanged.
- numeric values are converted to decimal strings in the usual way
- boolean values are represented as "0" or "1"
- binary values are unchanged *TODO check HTTP header encoding rules*

Some HTTP headers get special treatment (e.g. `Content-Type`)

AMQP application-properties with a "http-" prefix get special treatment (e.g. `http-method`)

*TODO consider also message and delivery annotations, possibly additional message properties*

### AMQP message to HTTP request

Note this describes the *initial* translation from AMQP to HTTP by the
bridge. The HTTP request is then forwarded to Envoy's HTTP router and filters
which can add, remove or modify headers.

The AMQP message
* must have a non-empty `reply_to` to deliver the AMQP response.
* may have a `correlation_id`, if so it will be copied to the AMQP response.

The HTTP request line is formed as follows:
* Method = AMQP `subject` message property.
* URI = the first of the following which is not empty:
  * AMQP `to` message property
  * Target address of the link the message arrived on
  * "/"

AMQP application-properties become HTTP headers as described [above](#headers).

Special Headers:
* [Host:][host-header] Taken from the URI, or AMQP host if the URI has no host part.
* Content-Length: byte length of content
* Content-Type: see [above](#body)

[host-header]: https://www.w3.org/Protocols/rfc2616/rfc2616-sec14.html#sec14.23

### HTTP response to AMQP message or disposition

A successful HTTP response (2xx status code) is converted to an AMQP message as follows:

* `subject` = HTTP status code + reason, e.g. "200 OK" *TODO abuse of subject?*
* `to` address = `reply-to` address from the original AMQP request
* `correlation_id` = `correlation_id` from the original AMQP request
* `body` = single data section containing HTTP response body (see [above](#body))
* `content-type` = single data section containing HTTP response body (see [above](#body))

HTTP error code causes an unsuccessful AMQP disposition for the request message,
no response message is sent:

* 4xx (client error) - settled `rejected`
* 5xx (server error) - settled `modified`, `delivery-failed`

*TODO rejected/modified should carry HTTP status code/reason/body in error condition.*

HTTP information (1xx) and redirect (3xx) codes are handled by the bridge, the
are not visible to the AMQP side of the connection.

### HTTP request to AMQP request message

*TODO: UNDER CONSTRUCTION*

### AMQP response message or disposition to HTTP response

*TODO: UNDER CONSTRUCTION*

## Building

*TODO Building currently requires hacking on build flags - at your own peril*

Proton is all shared libraries, envoy is all static. One or both of those
conditions will be fixed before this is over: building a static proton lib
should be easy.  Making envoy load shared modules dynamically probably isn't
very hard and would be more rewarding.

## Testing

*TODO Need proper automated tests*

Right now you need to launch envoy manually like this:

    bazel build envoy  && bazel-bin/envoy -l debug -c amqp_bridge.yaml

And then run the ruby test, which includes an AMQP client and HTTP server

    ./test_amqp_client.rb :10000

The test client is in ruby for speed of development, it should be replaced with
properly integrated unit and integration tests using Envoy's testing framework
eventually.


<a name="todo"></a>
# TODO list

Complete the amqp-bridge
- [ ] Establish outbound AMQP connections. Requires Envoy upstream filters (envoyproxy/envoy#173)
- [ ] Make outbound AMQP links based on HTTP service configuration to advertise HTTP services
- [ ] Many FIXME and TODO comments in the code and this README.
- [ ] Timers and heartbeats
- [ ] Envoy to load shared modules (envoyproxy/envoy#2053)
- [ ] Network filter callback for downstream write (envoyproxy/envoy#3343)
- [ ] Use Envoy's HTTP codec - maturity, support for HTTP2
- [ ] Configuration of hard-coded values: credit window, container-id, dynamic-prefix etc.

Testing
- [ ] Automated unit and integration tests under bazel
- [ ] Multi-host tests in more realistic setting (including dispatch, kubernetes etc.)

Nice to have
- [ ] stream large message bodies (need proton codec to decode AMQP headers before completion)
- [ ] redirect proton logs to Envoy logs.

## Future/maybe:

*Eliminate double HTTP codec*: Presently the envoy.connection_manager is the only "boundary" filter between a raw Network::Filter chain and a HTTP filter chain. The amqp-bridge filter composes/parses HTTP by itself and exchanges HTTP bytes with the connection-manager, which must parse/compose again. Ideally the amqp-bridge would exchange Envoy HTTP data structures directly with the connection manager to eliminate the duplicated work. A similar argument applies for upstream filters. Needs some thought about best to hook these things together.

