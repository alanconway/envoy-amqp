prefix() { awk "\$0=\"$1 \"\$0"; }
kill_all() { kill $(jobs -p); }
trap "kill_all" EXIT            # envoy doesn't die on SIGHUP

run_envoy() {
    ../bazel-bin/envoy -l debug --log-format "$1 [%T.%e][%l] %v" --disable-hot-restart -c $1.yaml 2>&1 | fgrep '[amqp_'
}

run_envoy env1&
qdrouterd -c qdr2.conf 2>&1 | prefix qdr2&
run_envoy env3&
wait
