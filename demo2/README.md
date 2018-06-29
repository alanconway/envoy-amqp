# Host-routed demo

Routing HTTP requests via dispatch to different back-end services,
based on the host name used to connect to a single envoy proxy.

## What's running

$ sh run.sh # runs the following processes

envoy-front -> qdrouterd -> envoy1
                         -> envoy2

## Configuration files

envoy-front.yaml: routes to qdrouterd using different AMQP addresses
based on host:
- localhost -> "service1"
- 127.0.0.1 -> "service2"

qdrouterd.conf: connects to envoy1 and envoy2.

envoy1/2.yaml:
- subscribes to service1 or service2 on qdrouterd (as sidecar)
- forwards all requests to it's own admin port (a handy REST service for demo)

## The demonstration

Query the admin service of envoy1 or 2 by changing the host used to
connect to envoy-front.

$ curl -w "\n" localhost:8000/listeners # This goes to envoy1
["0.0.0.0:15672"]
$ curl -w "\n" 127.0.0.1:8000/listeners # This goes to envoy2
["0.0.0.0:25672"]

Note that you can query any URL in the admin service address space.
qdrouterd is configured dynamically by the envoy routers setting up
subscriber links, only one address per sidecar is needed.

[Future work: it would be nice if envoy could also initiate the connection
to qdrouterd. See the to-do list in ../README.md]

Here's what happens in detail:

1. qdrouterd connects to both envoy1 and envoy2
2. envoy1/2 AMQP bridge creates subscriber links to service1, service2
3. curl connects to envoy-front.
4. Envoy-front forwards requests to qdrouterd, using the correct link
   address depending on the host (127.0.0.1 or localhost).
   It creates a dynamic-source "reply-to" address for replies.
5. qdrouterd routes addresses service1 and service2 as normal,
   traffic goes to envoy1 or envoy2 as appropriate
6. envoy1/2 forward all requests to their own "admin" ports.
7. The "to" address path is unaltered from the original URL,
   so the REST request is interpreted as normal by the admin service
8. REST responses are sent by envoy1/2 back to qdrouterd via ANONYMOUS_RELAY
   Their "to" address is the request's "reply-to" address.
8. qdrouterd routes back to envoy-front using "reply-to" (now "to") address
9. envoy-front responds to the curl client

Tah Dah!



