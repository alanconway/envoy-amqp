#!/bin/bash
set -e
envoy -l debug -c amqp_bridge.yaml --disable-hot-restart &
PID=$!
trap "kill -9 $PID" EXIT

# Wait for envoy to be listening on both test ports
listening() { nc -z localhost $1; }
until listening 18000 && listening 15672; do sleep .1; done

export RUBYLIB=external/proton/ruby/lib:external/proton/bld/ruby
for TEST in "$@"; do
    ruby $TEST -v
done

kill $PID
wait
trap "" EXIT

