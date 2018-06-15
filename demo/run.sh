run_envoy() {
    ../bazel-bin/envoy --disable-hot-restart -c $1.yaml 2>&1 | tee /tmp/$1.log
}
run_envoy env1&
run_envoy env3&
qdrouterd -c qdr2.conf 2>&1 | tee /tmp/qdr2.log&

wait -n || exit 1
