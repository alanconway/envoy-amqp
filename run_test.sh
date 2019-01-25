#!/bin/bash
set -e
envoy -c amqp_bridge.yaml --disable-hot-restart &
PID=$!
trap "kill -9 $PID" EXIT

# Wait for envoy to be listening on both test ports
listening() { nc -z localhost $1; }
until listening 9901 && listening 18000 && listening 15672; do
    kill -0 $PID || { echo "envoy exited"; exit 1; }
    sleep .1;
done
# curl http://localhost:9901/logging?filter=debug

export RUBYLIB=external/proton/ruby/lib:external/proton/ruby
for TEST in "$@"; do
    ruby $TEST -v
done

kill $PID
wait
trap "" EXIT

