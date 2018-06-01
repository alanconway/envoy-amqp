#!/bin/bash
set -e
envoy -l debug -c amqp_bridge.yaml &
PID=$!
trap "kill -9 $PID" EXIT
RUBYLIB=external/proton/ruby/lib:external/proton/bld/ruby ruby test_amqp_client.rb -v
