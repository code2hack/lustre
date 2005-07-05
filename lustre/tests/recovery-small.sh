#!/bin/sh

set -e

#         bug  2986 5494
ALWAYS_EXCEPT="20b  24"


LUSTRE=${LUSTRE:-`dirname $0`/..}

. $LUSTRE/tests/test-framework.sh

init_test_env $@

. ${CONFIG:=$LUSTRE/tests/cfg/local.sh}

build_test_filter


# Allow us to override the setup if we already have a mounted system by
# setting SETUP=" " and CLEANUP=" "
SETUP=${SETUP:-"setup"}
CLEANUP=${CLEANUP:-"cleanup"}
FORCE=${FORCE:-"--force"}

make_config() {
    rm -f $XMLCONFIG
    add_mds mds --dev $MDSDEV --size $MDSSIZE
    add_lov lov1 mds --stripe_sz $STRIPE_BYTES \
	--stripe_cnt $STRIPES_PER_OBJ --stripe_pattern 0
    add_ost ost --lov lov1 --dev $OSTDEV --size $OSTSIZE
    add_ost ost2 --lov lov1 --dev ${OSTDEV}-2 --size $OSTSIZE
    add_client client mds --lov lov1 --path $MOUNT
}

setup() {
    make_config
    start ost --reformat $OSTLCONFARGS 
    start ost2 --reformat $OSTLCONFARGS 
    [ "$DAEMONFILE" ] && $LCTL debug_daemon start $DAEMONFILE $DAEMONSIZE
    start mds $MDSLCONFARGS --reformat
    grep " $MOUNT " /proc/mounts || zconf_mount `hostname`  $MOUNT
}

cleanup() {
    zconf_umount `hostname` $MOUNT
    stop mds ${FORCE} $MDSLCONFARGS
    stop ost2 ${FORCE}
    stop ost ${FORCE} --dump $TMP/recovery-small-`hostname`.log
}

replay() {
    do_mds "sync"
    do_mds 'echo -e "device \$mds1\\nprobe\\nnotransno\\nreadonly" | lctl'
    do_client "$1" &
    shutdown_mds -f
    start_mds
    wait
    do_client "df -h $MOUNT" # trigger failover, if we haven't already
}

if [ ! -z "$EVAL" ]; then
    eval "$EVAL"
    exit $?
fi

if [ "$ONLY" == "cleanup" ]; then
    sysctl -w portals.debug=0 || true
    FORCE=--force cleanup
    exit
fi

REFORMAT=--reformat $SETUP
unset REFORMAT

[ "$ONLY" == "setup" ] && exit

test_1() {
    drop_request "mcreate $MOUNT/1"  || return 1
    drop_reint_reply "mcreate $MOUNT/2"    || return 2
}
run_test 1 "mcreate: drop req, drop rep"

test_2() {
    drop_request "tchmod 111 $MOUNT/2"  || return 1
    drop_reint_reply "tchmod 666 $MOUNT/2"    || return 2
}
run_test 2 "chmod: drop req, drop rep"

test_3() {
    drop_request "statone $MOUNT/2" || return 1
    drop_reply "statone $MOUNT/2"   || return 2
}
run_test 3 "stat: drop req, drop rep"

test_4() {
    do_facet client "cp /etc/resolv.conf $MOUNT/resolv.conf" || return 1
    drop_request "cat $MOUNT/resolv.conf > /dev/null"   || return 2
    drop_reply "cat $MOUNT/resolv.conf > /dev/null"     || return 3
}
run_test 4 "open: drop req, drop rep"

test_5() {
    drop_request "mv $MOUNT/resolv.conf $MOUNT/renamed" || return 1
    drop_reint_reply "mv $MOUNT/renamed $MOUNT/renamed-again" || return 2
    do_facet client "checkstat -v $MOUNT/renamed-again"  || return 3
}
run_test 5 "rename: drop req, drop rep"

test_6() {
    drop_request "mlink $MOUNT/renamed-again $MOUNT/link1" || return 1
    drop_reint_reply "mlink $MOUNT/renamed-again $MOUNT/link2"   || return 2
}
run_test 6 "link: drop req, drop rep"

test_7() {
    drop_request "munlink $MOUNT/link1"   || return 1
    drop_reint_reply "munlink $MOUNT/link2"     || return 2
}
run_test 7 "unlink: drop req, drop rep"

#bug 1423
test_8() {
    drop_reint_reply "touch $MOUNT/renamed"    || return 1
}
run_test 8 "touch: drop rep (bug 1423)"

#bug 1420
test_9() {
    pause_bulk "cp /etc/profile $MOUNT"       || return 1
    do_facet client "cp /etc/termcap $MOUNT"  || return 2
    do_facet client "sync"
    do_facet client "rm $MOUNT/termcap $MOUNT/profile" || return 3
}
run_test 9 "pause bulk on OST (bug 1420)"

#bug 1521
test_10() {
    do_facet client mcreate $MOUNT/$tfile        || return 1
    drop_bl_callback "chmod 0777 $MOUNT/$tfile"  || return 2
    # wait for the mds to evict the client
    #echo "sleep $(($TIMEOUT*2))"
    #sleep $(($TIMEOUT*2))
    do_facet client touch $MOUNT/$tfile || echo "touch failed, evicted"
    do_facet client checkstat -v -p 0777 $MOUNT/$tfile  || return 3
    do_facet client "munlink $MOUNT/$tfile"
}
run_test 10 "finish request on server after client eviction (bug 1521)"

#bug 2460
# wake up a thread waiting for completion after eviction
test_11(){
    do_facet client multiop $MOUNT/$tfile Ow  || return 1
    do_facet client multiop $MOUNT/$tfile or  || return 2

    cancel_lru_locks OSC

    do_facet client multiop $MOUNT/$tfile or  || return 3
    drop_bl_callback multiop $MOUNT/$tfile Ow  || 
        echo "client evicted, as expected"

    do_facet client munlink $MOUNT/$tfile  || return 4
}
run_test 11 "wake up a thread waiting for completion after eviction (b=2460)"

#b=2494
test_12(){
    $LCTL mark multiop $MOUNT/$tfile OS_c 
    do_facet mds "sysctl -w lustre.fail_loc=0x115"
    clear_failloc mds $((TIMEOUT * 2)) &
    multiop $MOUNT/$tfile OS_c  &
    PID=$!
#define OBD_FAIL_MDS_CLOSE_NET           0x115
    sleep 2
    kill -USR1 $PID
    echo "waiting for multiop $PID"
    wait $PID || return 2
    do_facet client munlink $MOUNT/$tfile  || return 3
}
run_test 12 "recover from timed out resend in ptlrpcd (b=2494)"

# Bug 113, check that readdir lost recv timeout works.
test_13() {
    mkdir /mnt/lustre/readdir || return 1
    touch /mnt/lustre/readdir/newentry || return
# OBD_FAIL_MDS_READPAGE_NET|OBD_FAIL_ONCE
    do_facet mds "sysctl -w lustre.fail_loc=0x80000104"
    ls /mnt/lustre/readdir || return 3
    do_facet mds "sysctl -w lustre.fail_loc=0"
    rm -rf /mnt/lustre/readdir || return 4
}
run_test 13 "mdc_readpage restart test (bug 1138)"

# Bug 113, check that readdir lost send timeout works.
test_14() {
    mkdir /mnt/lustre/readdir
    touch /mnt/lustre/readdir/newentry
# OBD_FAIL_MDS_SENDPAGE|OBD_FAIL_ONCE
    do_facet mds "sysctl -w lustre.fail_loc=0x80000106"
    ls /mnt/lustre/readdir || return 1
    do_facet mds "sysctl -w lustre.fail_loc=0"
}
run_test 14 "mdc_readpage resend test (bug 1138)"

test_15() {
    do_facet mds "sysctl -w lustre.fail_loc=0x80000128"
    touch $DIR/$tfile && return 1
    return 0
}
run_test 15 "failed open (-ENOMEM)"

READ_AHEAD=`cat /proc/fs/lustre/llite/*/max_read_ahead_mb | head -n 1`
stop_read_ahead() {
   for f in /proc/fs/lustre/llite/*/max_read_ahead_mb; do 
      echo 0 > $f
   done
}

start_read_ahead() {
   for f in /proc/fs/lustre/llite/*/max_read_ahead_mb; do 
      echo $READ_AHEAD > $f
   done
}

test_16() {
    do_facet client cp /etc/termcap $MOUNT
    sync
    stop_read_ahead

#define OBD_FAIL_PTLRPC_BULK_PUT_NET 0x504 | OBD_FAIL_ONCE
    sysctl -w lustre.fail_loc=0x80000504
    cancel_lru_locks OSC
    # will get evicted here
    do_facet client "cmp /etc/termcap $MOUNT/termcap"  && return 1
    sysctl -w lustre.fail_loc=0
    # give recovery a chance to finish (shouldn't take long)
    sleep $TIMEOUT
    do_facet client "cmp /etc/termcap $MOUNT/termcap"  || return 2
    start_read_ahead
}
run_test 16 "timeout bulk put, evict client (2732)"

test_17() {
    # OBD_FAIL_PTLRPC_BULK_GET_NET 0x0503 | OBD_FAIL_ONCE
    # client will get evicted here
    sysctl -w lustre.fail_loc=0x80000503
    # need to ensure we send an RPC
    do_facet client cp /etc/termcap $DIR/$tfile
    sync

    sleep $TIMEOUT
    sysctl -w lustre.fail_loc=0
    do_facet client "df $DIR"
    # expect cmp to fail
    do_facet client "cmp /etc/termcap $DIR/$tfile"  && return 1
    do_facet client "rm $DIR/$tfile" || return 2
    return 0
}
run_test 17 "timeout bulk get, evict client (2732)"

test_18a() {
    do_facet client mkdir -p $MOUNT/$tdir
    f=$MOUNT/$tdir/$tfile

    cancel_lru_locks OSC
    pgcache_empty || return 1

    # 1 stripe on ost2
    lfs setstripe $f $((128 * 1024)) 1 1

    do_facet client cp /etc/termcap $f
    sync
    local osc2_dev=`$LCTL device_list | \
	awk '(/ost2.*client_facet/){print $4}' `
    $LCTL --device %$osc2_dev deactivate
    # my understanding is that there should be nothing in the page
    # cache after the client reconnects?     
    rc=0
    pgcache_empty || rc=2
    $LCTL --device %$osc2_dev activate
    rm -f $f
    return $rc
}
run_test 18a "manual ost invalidate clears page cache immediately"

test_18b() {
    do_facet client mkdir -p $MOUNT/$tdir
    f=$MOUNT/$tdir/$tfile
    f2=$MOUNT/$tdir/${tfile}-2

    cancel_lru_locks OSC
    pgcache_empty || return 1

    # shouldn't have to set stripe size of count==1
    lfs setstripe $f $((128 * 1024)) 0 1
    lfs setstripe $f2 $((128 * 1024)) 0 1

    do_facet client cp /etc/termcap $f
    sync
    # just use this write to trigger the client's eviction from the ost
# OBD_FAIL_PTLRPC_BULK_GET_NET|OBD_FAIL_ONCE
    sysctl -w lustre.fail_loc=0x80000503
    do_facet client dd if=/dev/zero of=$f2 bs=4k count=1
    sync
    sysctl -w lustre.fail_loc=0
    # allow recovery to complete
    sleep $((TIMEOUT + 2))
    # my understanding is that there should be nothing in the page
    # cache after the client reconnects?     
    rc=0
    pgcache_empty || rc=2
    rm -f $f $f2
    return $rc
}
run_test 18b "eviction and reconnect clears page cache (2766)"

test_19a() {
    f=$MOUNT/$tfile
    do_facet client mcreate $f        || return 1
    drop_ldlm_cancel "chmod 0777 $f"  || echo evicted

    do_facet client checkstat -v -p 0777 $f  || echo evicted
    # let the client reconnect
    sleep 5
    do_facet client "munlink $f"
}
run_test 19a "test expired_lock_main on mds (2867)"

test_19b() {
    f=$MOUNT/$tfile
    do_facet client multiop $f Ow  || return 1
    do_facet client multiop $f or  || return 2

    cancel_lru_locks OSC

    do_facet client multiop $f or  || return 3
    drop_ldlm_cancel multiop $f Ow  || echo "client evicted, as expected"

    do_facet client munlink $f  || return 4
}
run_test 19b "test expired_lock_main on ost (2867)"

test_20a() {	# bug 2983 - ldlm_handle_enqueue cleanup
	mkdir -p $DIR/$tdir
	multiop $DIR/$tdir/${tfile} O_wc &
	MULTI_PID=$!
	sleep 1
	cancel_lru_locks OSC
#define OBD_FAIL_LDLM_ENQUEUE_EXTENT_ERR 0x308
	do_facet ost sysctl -w lustre.fail_loc=0x80000308
	kill -USR1 $MULTI_PID
	wait $MULTI_PID
	rc=$?
	[ $rc -eq 0 ] && error "multiop didn't fail enqueue: rc $rc" || true
}
run_test 20a "ldlm_handle_enqueue error (should return error)" 

test_20b() {	# bug 2986 - ldlm_handle_enqueue error during open
	mkdir -p $DIR/$tdir
	touch $DIR/$tdir/${tfile}
	cancel_lru_locks OSC
#define OBD_FAIL_LDLM_ENQUEUE_EXTENT_ERR 0x308
	do_facet ost sysctl -w lustre.fail_loc=0x80000308
	dd if=/etc/hosts of=$DIR/$tdir/$tfile && \
		error "didn't fail open enqueue" || true
}
run_test 20b "ldlm_handle_enqueue error (should return error)"

#b_cray run_test 21a "drop close request while close and open are both in flight"
#b_cray run_test 21b "drop open request while close and open are both in flight"
#b_cray run_test 21c "drop both request while close and open are both in flight"
#b_cray run_test 21d "drop close reply while close and open are both in flight"
#b_cray run_test 21e "drop open reply while close and open are both in flight"
#b_cray run_test 21f "drop both reply while close and open are both in flight"
#b_cray run_test 21g "drop open reply and close request while close and open are both in flight"
#b_cray run_test 21h "drop open request and close reply while close and open are both in flight"
#b_cray run_test 22 "drop close request and do mknod"
#b_cray run_test 23 "client hang when close a file after mds crash"

test_24() {	# bug 2248 - eviction fails writeback but app doesn't see it
	mkdir -p $DIR/$tdir
	cancel_lru_locks OSC
	multiop $DIR/$tdir/$tfile Owy_wyc &
	MULTI_PID=$!
	usleep 500
# OBD_FAIL_PTLRPC_BULK_GET_NET|OBD_FAIL_ONCE
	sysctl -w lustre.fail_loc=0x80000503
	usleep 500
	kill -USR1 $MULTI_PID
	wait $MULTI_PID
	rc=$?
	sysctl -w lustre.fail_loc=0x0
	client_reconnect
	[ $rc -eq 0 ] && error "multiop didn't fail fsync: rc $rc" || true
}
run_test 24 "fsync error (should return error)"

test_26() {      # bug 5921 - evict dead exports 
# this test can only run from a client on a separate node.
	[ "`lsmod | grep obdfilter`" ] && \
	    echo "skipping test 26 (local OST)" && return
	[ "`lsmod | grep mds`" ] && \
	    echo "skipping test 26 (local MDS)" && return
	OST_FILE=/proc/fs/lustre/obdfilter/ost_svc/num_exports
        OST_EXP="`do_facet ost cat $OST_FILE`"
	OST_NEXP1=`echo $OST_EXP | cut -d' ' -f2`
	echo starting with $OST_NEXP1 OST exports
# OBD_FAIL_PTLRPC_DROP_RPC 0x505
	do_facet client sysctl -w lustre.fail_loc=0x505
	# evictor takes up to 2.25x to evict.  But if there's a 
	# race to start the evictor from various obds, the loser
	# might have to wait for the next ping.
	echo Waiting for $(($TIMEOUT * 4)) secs
	sleep $(($TIMEOUT * 4))
        OST_EXP="`do_facet ost cat $OST_FILE`"
	OST_NEXP2=`echo $OST_EXP | cut -d' ' -f2`
	echo ending with $OST_NEXP2 OST exports
	do_facet client sysctl -w lustre.fail_loc=0x0
        [ $OST_NEXP1 -le $OST_NEXP2 ] && error "client not evicted"
	return 0
}
run_test 26 "evict dead exports"

test_27() {
	[ "`lsmod | grep mds`" ] || \
	    { echo "skipping test 27 (non-local MDS)" && return 0; }
	mkdir -p $DIR/$tdir
	writemany -q -a $DIR/$tdir/$tfile 0 5 &
	CLIENT_PID=$!
	sleep 1
	FAILURE_MODE="SOFT"
	facet_failover mds
#define OBD_FAIL_OSC_SHUTDOWN            0x407
	sysctl -w lustre.fail_loc=0x80000407
	# need to wait for reconnect
	echo -n waiting for fail_loc
	while [ `sysctl -n lustre.fail_loc` -eq -2147482617 ]; do
	    sleep 1
	    echo -n .
	done
	facet_failover mds
	#no crashes allowed!
        kill -USR1 $CLIENT_PID
	wait $CLIENT_PID 
	true
}
run_test 27 "fail LOV while using OSC's"

test_28() {      # bug 6086 - error adding new clients
	do_facet client mcreate $MOUNT/$tfile        || return 1
	drop_bl_callback "chmod 0777 $MOUNT/$tfile"  || return 2
	#define OBD_FAIL_MDS_ADD_CLIENT 0x12f
	do_facet mds sysctl -w lustre.fail_loc=0x8000012f
	# fail once (evicted), reconnect fail (fail_loc), ok
	df || (sleep 1; df) || (sleep 1; df) || error "reconnect failed"
	rm -f $MOUNT/$tfile
	fail mds		# verify MDS last_rcvd can be loaded
}
run_test 28 "handle error adding new clients (bug 6086)"

test_50() {
	mkdir -p $DIR/$tdir
	# put a load of file creates/writes/deletes
	writemany -q $DIR/$tdir/$tfile 0 5 &
	CLIENT_PID=$!
	echo writemany pid $CLIENT_PID
	sleep 10
	FAILURE_MODE="SOFT"
	fail mds
	# wait for client to reconnect to MDS
	sleep 60
	fail mds
	sleep 60
	fail mds
	# client process should see no problems even though MDS went down
	sleep $TIMEOUT
        kill -USR1 $CLIENT_PID
	wait $CLIENT_PID 
	rc=$?
	echo writemany returned $rc
	#these may fail because of eviction due to slow AST response.
	return $rc
}
run_test 50 "failover MDS under load"

test_51() {
	mkdir -p $DIR/$tdir
	# put a load of file creates/writes/deletes
	writemany -q $DIR/$tdir/$tfile 0 5 &
	CLIENT_PID=$!
	sleep 1
	FAILURE_MODE="SOFT"
	facet_failover mds
	# failover at various points during recovery
	SEQ="1 5 10 $(seq $TIMEOUT 5 $(($TIMEOUT+10)))"
        echo will failover at $SEQ
        for i in $SEQ
          do
          echo failover in $i sec
          sleep $i
          facet_failover mds
        done
	# client process should see no problems even though MDS went down
	# and recovery was interrupted
	sleep $TIMEOUT
        kill -USR1 $CLIENT_PID
	wait $CLIENT_PID 
	rc=$?
	echo writemany returned $rc
	return $rc
}
run_test 51 "failover MDS during recovery"

test_52_guts() {
	do_facet client "writemany -q -a $DIR/$tdir/$tfile 300 5" &
	CLIENT_PID=$!
	echo writemany pid $CLIENT_PID
	sleep 10
	FAILURE_MODE="SOFT"
	fail ost
	rc=0
	wait $CLIENT_PID || rc=$?
	# active client process should see an EIO for down OST
	[ $rc -eq 5 ] && { echo "writemany correctly failed $rc" && return 0; }
	# but timing or failover setup may allow success
	[ $rc -eq 0 ] && { echo "writemany succeeded" && return 0; }
	echo "writemany returned $rc"
	return $rc
}

test_52() {
	mkdir -p $DIR/$tdir
	test_52_guts
	rc=$?
	[ $rc -ne 0 ] && { return $rc; }
	# wait for client to reconnect to OST
	sleep 30
	test_52_guts
	rc=$?
	[ $rc -ne 0 ] && { return $rc; }
	sleep 30
	test_52_guts
	rc=$?
	client_reconnect
	#return $rc
}
run_test 52 "failover OST under load"


FORCE=--force $CLEANUP
