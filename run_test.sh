#!/bin/bash
set -e

# Check if a port is listening
listening() {
    ss -Htln | egrep -q -m1 '^LISTEN\s+[0-9]+\s+[0-9]+\s+.*:'$1'\s'
}

startup() {
    envoy -l warning -c amqp_bridge.yaml --disable-hot-restart &
    PID=$!
    until listening 15672 && listening 18000 && listening 9901; do
        sleep .1
    done
    trap "kill -9 $PID" EXIT
}

startup
RUBYLIB=external/proton/ruby/lib:external/proton/bld/ruby ruby "$@"
shutdown
kill $PID
wait
trap "" EXIT
