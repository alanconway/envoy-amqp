# Run an envoy/dispatch/envoy sandwich

set -e

sudo pkill -9 envoy || true
pkill -9 qdrouterd || true
sleep 1

LOGS=logs
mkdir -p $LOGS
rm -f $LOGS/*

background() {
    NAME=$1; shift;
    { "$@" > $LOGS/$NAME.log 2>&1 || { echo "ERROR - $NAME $?"; exit 1; } } &
}

run_envoy() {
    background "$@" ../bazel-bin/envoy -l debug --disable-hot-restart -c $1.yaml
}

run_envoy front sudo
run_envoy ernie
run_envoy bert
background qdrouterd qdrouterd -c qdrouterd.conf

tail -F $LOGS/*.log&
wait -n %1 %2 %3 %4 || echo "something failed"
