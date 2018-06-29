# Run an envoy/dispatch/envoy sandwich

pkill -9 envoy
pkill -9 qdrouterd
sleep 1

LOGS=/tmp/demo
mkdir -p $LOGS
rm -f $LOGS/*

background() {
    NAME=$1; shift;
    { "$@" > $LOGS/$NAME.log 2>&1 || { echo "ERROR - $NAME $?"; exit 1; } } &
}

run_envoy() {
    background $1 ../bazel-bin/envoy -l debug --disable-hot-restart -c $1.yaml
}

run_envoy envoy-front
run_envoy envoy1
run_envoy envoy2
background qdrouterd qdrouterd -c qdrouterd.conf
tail -F $LOGS/*.log

wait
