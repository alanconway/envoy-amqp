# Host-routed demo

Using an envoy router to route HTTP requests by host name to REST services,
using Qpid Dispatch Router to route some of the requests.

To run the example you need permission to listen on port 80 and you must any HTTP server already listening there.
Alternatively you can edit the demo configuration and change the port.

This example needs the following aliases in /etc/hosts:

    127.0.0.1  ernie.local bert.local grover.local builds.apache.org

These are used to route messages by virtual host name:
- ernie.local - routes to Dispatch with AMQP address "ernie"
- bert.local  - routes to Dispatch with AMQP address "bert"
- grover.local - routes to HTTP site echo.jsontest.com

$ sh run.sh # runs the following processes

envoy(front.yaml) (on port 80) -> qdrouterd -> envoy(ernie.yaml)
                                            -> envoy(bert.yaml)

## The Demonstration

Query issues.apache.org via host "ernie.local"

    curl -s 'http://ernie.local/jira/rest/api/2/search?jql=assignee=aconway&maxResults=1&fields=summary' | json_reformat

Query bugzilla.mozilla.org for bugs via host "bert.local"

    curl -s 'http://bert.local/rest/bug/9999?include_fields=summary,id' | json_reformat
    curl -s 'http://bert.local/rest/bug/1234?include_fields=summary,id' | json_reformat

Query echo.jsontest.com via host "grover.local":

    curl -v http://grover.local/hello/world

## Things to note

### Dynamic configuration of Dispatch

Dispatch router has no configuration except to establish connections. The envoy routers create links according to their configuration, and Dispatch automatically propagates those link addresses in the AMQP network.

Future work: envoy needs to be able to initiate the connection to qdrouterd to further reduce static configuration. See the to-do list in ../README.MD

### Dispatch is not aware of every URI

A REST service consists of many URIs with a common prefix, new URIs are created as the service runs - for example in the Bugzilla REST API every bug has it's own URI. We can't statically map individual URIs to AMQP addresses because the set of URIs is open.

Instead, each AMQP address known to dispatch represents a HTTP "destination" which identifies an envoy server-side endpoint. The AMQP to: address on each message provides the full URI path to be interpreted by the envoy server or other HTTP service. Each AMQP addresses is like a "named relay" that tunnels HTTP requests to many related URIs.

On the client side, envoy can use host, URI prefix, query and HTTP headers to select an AMQP address. On the server side, envoy can rewrite the host and path prefix to match local expectations before forwarding the HTTP requests.

Dispatch routing allows the envoy server-side endpoints to be relocated, load-balanced and so on without updating client-side configuration.


