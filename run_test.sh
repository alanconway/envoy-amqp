#!/bin/bash
set -e

envoy -l warning -c amqp_bridge.yaml --disable-hot-restart &
PID=$!
trap "kill -9 $PID" EXIT

# Wait for envoy to be listening on both test ports
listening() { nc -v -z localhost $1; }
until listening 18000 && listening 15672; do sleep .1; done

test -d external && DIR=external/ # Run from bazel test or from this directory
export RUBYLIB=${DIR}proton/ruby/lib:${DIR}proton/bld/ruby
ruby "$@"

kill $PID
wait
trap "" EXIT
