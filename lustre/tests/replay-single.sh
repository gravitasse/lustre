#!/bin/sh

set -e

LUSTRE=${LUSTRE:-`dirname $0`/..}
LTESTDIR=${LTESTDIR:-$LUSTRE/../ltest}
PATH=$PATH:$LUSTRE/utils:$LUSTRE/tests

RLUSTRE=${RLUSTRE:-$LUSTRE}
RPWD=${RPWD:-$PWD}

. $LTESTDIR/functional/llite/common/common.sh

# XXX I wish all this stuff was in some default-config.sh somewhere
MOUNTPT=${MOUNTPT:-/mnt/lustre}
MDSDEV=${MDSDEV:-/tmp/mds-`hostname`}
MDSSIZE=${MDSSIZE:-100000}
OSTDEV=${OSTDEV:-/tmp/ost-`hostname`}
OSTSIZE=${OSTSIZE:-100000}
UPCALL=${UPCALL:-$PWD/replay-single-upcall.sh}
FSTYPE=${FSTYPE:-ext3}
TIMEOUT=${TIMEOUT:-5}

start() {
    facet=$1
    shift
    lconf --node ${facet}_facet $@ replay-single.xml
}

stop() {
    facet=$1
    shift
    lconf --node ${facet}_facet $@ -d replay-single.xml
}

replay_barrier() {
    local dev=$1
    sync
    lctl --device %${dev}1 readonly
    lctl --device %${dev}1 notransno
}

fail() {
    stop mds -f --failover
    start mds
    df $MOUNTPT
}

do_lmc() {
    lmc -m replay-single.xml $@
}

add_facet() {
    local facet=$1
    shift
    do_lmc --add node --node ${facet}_facet $@ --timeout $TIMEOUT
    do_lmc --add net --node ${facet}_facet --nid localhost --nettype tcp
}

gen_config() {
    rm -f replay-single.xml
    add_facet mds
    add_facet ost
    add_facet client --lustre_upcall $UPCALL
    do_lmc --add mds --node mds_facet --mds mds1 --dev $MDSDEV --size $MDSSIZE
    do_lmc --add ost --node ost_facet --ost ost1 --dev $OSTDEV --size $OSTSIZE
    do_lmc --add mtpt --node client_facet --path $MOUNTPT --mds mds1 --ost ost1
}

error() {
    echo '**** FAIL:' $@
    exit 1
}

EQUALS="======================================================================"

run_test() {
    testnum=$1
    message=$2
    
    # Pretty tests run faster.
    echo -n '=====' $testnum: $message
    local suffixlen=$((65 - `echo -n $2 | wc -c | awk '{print $1}'`))
    printf ' %.*s\n' $suffixlen $EQUALS

    test_${testnum} || error "test_$testnum failed with $?"
}

gen_config
start mds --reformat $MDSLCONFARGS
start ost --reformat $OSTLCONFARGS
start client --gdb $CLIENTLCONFARGS

test_1() {
    replay_barrier mds
    mcreate $MOUNTPT/f1
    fail
    ls $MOUNTPT/f1
    rm $MOUNTPT/f1
}
run_test 1 "simple create"

test_2() {
    replay_barrier mds
    mkdir $MOUNTPT/d2
    mcreate $MOUNTPT/d2/f2
    fail
    ls $MOUNTPT/d2/fs
    rm -fr $MOUNTPT/d2
}
run_test 2 "mkdir + contained create"

test_3() {
    mkdir $MOUNTPT/d3
    replay_barrier mds
    mcreate $MOUNTPT/d3/f3
    fail
    ls $MOUNTPT/d3/f3
    rm -fr $MOUNTPT/d3
}
run_test 3 "mkdir |X| contained create"

test_4() {
    multiop $MOUNTPT/f4 mo_c &
    MULTIPID=$!
    sleep 1
    fail
    ls $MOUNTPT/f4
    kill -USR1 $MULTIPID
    wait
    rm $MOUNTPT/f4
}
run_test 4 "open |X| close"

stop client $CLIENTLCONFARGS
stop ost
stop mds $MDSLCONFARGS
