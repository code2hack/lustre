#!/bin/sh

set -e
export REFORMAT=""
export VERBOSE=false

# eg, assert_env LUSTRE MDSNODES OSTNODES CLIENTS
assert_env() {
    local failed=""
    for name in $@; do
      if [ -z "${!name}" ]; then
	  echo "$0: $name must be set"
          failed=1
      fi
    done
    [ $failed ] && exit 1 || true
}

usage() {
    echo "usage: $0 [-r] [-f cfgfile]"
    echo "       -r: reformat"

    exit
}

init_test_env() {
    export LUSTRE=`absolute_path $LUSTRE`
    export TESTSUITE=`basename $0 .sh`
    export XMLCONFIG=${XMLCONFIG:-${TESTSUITE}.xml}
    export LTESTDIR=${LTESTDIR:-$LUSTRE/../ltest}

    [ -d /r ] && test "x$ROOT" = "x" && export ROOT=/r
    export TMP=${TMP:-$ROOT/tmp}

    export PATH=:$PATH:$LUSTRE/utils:$LUSTRE/tests
    export LLMOUNT=${LLMOUNT:-"llmount"}
    export LCONF=${LCONF:-"lconf"}
    export LMC=${LMC:-"lmc"}
    export LCTL=${LCTL:-"$LUSTRE/utils/lctl"}
    export CHECKSTAT="${CHECKSTAT:-checkstat} "
    DEF_FSTYPE=`test "x$(uname -r | grep -o '2.6')" = "x2.6" && echo "ldiskfs" || echo "ext3"`
    export FSTYPE=${FSTYPE:-$DEF_FSTYPE}
    #used only if FSTYPE == smfs, otherwise ignored by lconf
    MDS_BACKFSTYPE=${MDS_BACKFSTYPE:-$DEF_FSTYPE}
    OST_BACKFSTYPE=${OST_BACKFSTYPE:-$DEF_FSTYPE}

    export SECURITY=${SECURITY:-"null"}

    # Paths on remote nodes, if different 
    export RLUSTRE=${RLUSTRE:-$LUSTRE}
    export RPWD=${RPWD:-$PWD}

    # command line
    
    while getopts "rvf:" opt $*; do 
	case $opt in
	    f) CONFIG=$OPTARG;;
	    r) REFORMAT=--reformat;;
	    v) VERBOSE=true;;
	    \?) usage;;
	esac
    done
    
    shift $((OPTIND - 1))
    ONLY=${ONLY:-$*}
    
    # save the name of the config file for the upcall
    echo "XMLCONFIG=$LUSTRE/tests/$XMLCONFIG"  > $LUSTRE/tests/XMLCONFIG
#    echo "CONFIG=`canonical_path $CONFIG`"  > $LUSTRE/tests/CONFIG
}

. krb5_env.sh

# Facet functions
start() {
    facet=$1
    shift
    active=`facet_active $facet`
    do_facet $facet $LCONF --select ${facet}_svc=${active}_facet \
        --node ${active}_facet  --ptldebug $PTLDEBUG --subsystem $SUBSYSTEM \
        --mds_sec $SECURITY $@ $XMLCONFIG
}

stop() {
    facet=$1
    active=`facet_active $facet`
    shift
    do_facet $facet $LCONF --select ${facet}_svc=${active}_facet \
        --node ${active}_facet  --ptldebug $PTLDEBUG --subsystem $SUBSYSTEM \
        $@ --cleanup $XMLCONFIG
}

zconf_mount() {
    client=$1
    mnt=$2

    do_node $client mkdir $mnt 2> /dev/null || :

    if [ -x /sbin/mount.lustre ] ; then
	do_node $client mount -t lustre -o mds_sec=$SECURITY,nettype=$NETTYPE \
                `facet_active_host mds1`:/mds1_svc/client_facet $mnt || return 2
    else
       # this is so cheating
       do_node $client $LCONF --nosetup --node client_facet $XMLCONFIG  > /dev/null || return 2
       do_node $client $LLMOUNT `facet_active_host mds1`:/mds1_svc/client_facet $mnt \
               -o mds_sec=$SECURITY,nettype=$NETTYPE|| return 4
    fi

    [ -d /r ] && $LCTL modules > /r/tmp/ogdb-`hostname`
    return 0
}

zconf_umount() {
    client=$1
    mnt=$2
    [ "$3" ] && force=-f
    do_node $client umount $force  $mnt || :
    do_node $client $LCONF --cleanup --nosetup --node client_facet $XMLCONFIG > /dev/null || :
}

shutdown_facet() {
    facet=$1
    if [ "$FAILURE_MODE" = HARD ]; then
       $POWER_DOWN `facet_active_host $facet`
       sleep 2 
    elif [ "$FAILURE_MODE" = SOFT ]; then
       stop $facet --force --failover --nomod
    fi
}

reboot_facet() {
    facet=$1
    if [ "$FAILURE_MODE" = HARD ]; then
       $POWER_UP `facet_active_host $facet`
    fi
}

wait_for_host() {
   HOST=$1
   check_network  $HOST 900
   while ! do_node $HOST "ls -d $LUSTRE " > /dev/null; do sleep 5; done
}

wait_for() {
   facet=$1
   HOST=`facet_active_host $facet`
   wait_for_host $HOST
}

client_df() {
    # not every config has many clients
    if [ ! -z "$CLIENTS" ]; then
	$PDSH $CLIENTS "df $MOUNT" > /dev/null
    fi
}

facet_failover() {
    facet=$1
    echo "Failing $facet node `facet_active_host $facet`"
    shutdown_facet $facet
    reboot_facet $facet
    client_df &
    DFPID=$!
    echo "df pid is $DFPID"
    change_active $facet
    TO=`facet_active_host $facet`
    echo "Failover $facet to $TO"
    wait_for $facet
    start $facet
}

replay_barrier() {
    local facet=$1
    do_facet $facet sync
    df $MOUNT
    do_facet $facet $LCTL --device %${facet}_svc readonly
    do_facet $facet $LCTL --device %${facet}_svc notransno
    do_facet $facet $LCTL mark "REPLAY BARRIER"
    $LCTL mark "REPLAY BARRIER"
}

replay_barrier_nodf() {
    local facet=$1
    do_facet $facet sync
    do_facet $facet $LCTL --device %${facet}_svc readonly
    do_facet $facet $LCTL --device %${facet}_svc notransno
    do_facet $facet $LCTL mark "REPLAY BARRIER"
    $LCTL mark "REPLAY BARRIER"
}

mds_evict_client() {
    UUID=`cat /proc/fs/lustre/mdc/*_MNT_*/uuid`
    do_facet mds "echo $UUID > /proc/fs/lustre/mds/mds1_svc/evict_client"
}

fail() {
    local facet=$1
    facet_failover $facet
    df $MOUNT || error "post-failover df: $?"
}

fail_drop() {
    local facet=$1
    local failcode=$2
    facet_failover $facet
    do_facet mds "echo $failcode > /proc/sys/lustre/fail_loc"
    df $MOUNT || error "post-failover df: $?"
    do_facet mds "echo 0 > /proc/sys/lustre/fail_loc"
}

fail_abort() {
    local facet=$1
    stop $facet --force --failover --nomod
    change_active $facet
    start $facet
    do_facet $facet lctl --device %${facet}_svc abort_recovery
    df $MOUNT || echo "first df failed: $?"
    sleep 1
    df $MOUNT || error "post-failover df: $?"
}

do_lmc() {
    $LMC -m ${XMLCONFIG} $@
}

h2gm () {
   if [ "$1" = "client" -o "$1" = "'*'" ]; then echo \'*\'; else
       $PDSH $1 $GMNALNID -l | cut -d\  -f2
   fi
}

h2tcp() {
   if [ "$1" = "client" -o "$1" = "'*'" ]; then echo \'*\'; else
   echo $1 
   fi
}
declare -fx h2tcp

h2elan() {
   if [ "$1" = "client" -o "$1" = "'*'" ]; then echo \'*\'; else
   echo $1 | sed 's/[^0-9]*//g'
   fi
}
declare -fx h2elan

h2openib() {
   if [ "$1" = "client" -o "$1" = "'*'" ]; then echo \'*\'; else
   echo $1 | sed 's/[^0-9]*//g'
   fi
}
declare -fx h2openib


facet_host() {
   local facet=$1
   varname=${facet}_HOST
   echo -n ${!varname}
}

facet_nid() {
   facet=$1
   HOST=`facet_host $facet`
   if [ -z "$HOST" ]; then
	echo "The env variable ${facet}_HOST must be set."
	exit 1
   fi
   echo `h2$NETTYPE $HOST`
}

facet_active() {
    local facet=$1
    local activevar=${facet}active

    if [ -f ./${facet}active ] ; then
        source ./${facet}active
    fi

    active=${!activevar}
    if [ -z "$active" ] ; then 
	echo -n ${facet}
    else
	echo -n ${active}
    fi
}

facet_active_host() {
    local facet=$1
    local active=`facet_active $facet`
    if [ "$facet" == client ]; then
	hostname
    else
	echo `facet_host $active`
    fi
}

change_active() {
    local facet=$1
    failover=${facet}failover 
    host=`facet_host $failover`
    [ -z "$host" ] && return
    curactive=`facet_active $facet`
    if [ -z "${curactive}" -o "$curactive" == "$failover" ] ; then
        eval export ${facet}active=$facet
    else
        eval export ${facet}active=$failover
    fi
    # save the active host for this facet
    activevar=${facet}active
    echo "$activevar=${!activevar}" > ./$activevar
}

do_node() {
    HOST=$1
    shift

    if $VERBOSE; then
	echo "CMD: $HOST $@"
	$PDSH $HOST $LCTL mark "$@" > /dev/null 2>&1 || :
    fi
    $PDSH $HOST "(PATH=$RLUSTRE/utils:$RLUSTRE/tests:/usr/sbin:/sbin:\$PATH; cd $RPWD; sh -c \"$@\")"
}

mds_list() {
    seq -f mds%g $MDSCOUNT
}

do_facet() {
    facet=$1
    shift

    if [ "$facet" == "mds" ]; then
	for mds in `mds_list`; do
           HOST=`facet_active_host $mds`
           do_node $HOST $@
	done
    else
	HOST=`facet_active_host $facet`
	do_node $HOST $@
    fi
}

add_facet() {
    local facet=$1
    shift
    echo "add facet $facet: `facet_host $facet`"
    do_lmc --add node --node ${facet}_facet $@ --timeout $TIMEOUT \
        --lustre_upcall $UPCALL --ptldebug $PTLDEBUG --subsystem $SUBSYSTEM
    do_lmc --add net --node ${facet}_facet --nid `facet_nid $facet` \
	--nettype $NETTYPE
}

add_mds() {
    facet=$1
    shift
    rm -f ${facet}active
    add_facet $facet
    do_lmc --add mds --node ${facet}_facet --mds ${facet}_svc --fstype $FSTYPE --backfstype $MDS_BACKFSTYPE $*
}

add_mdsfailover() {
    facet=$1
    shift
    add_facet ${facet}failover  --lustre_upcall $UPCALL
    do_lmc --add mds  --node ${facet}failover_facet --mds ${facet}_svc --fstype $FSTYPE --backfstype $MDS_BACKFSTYPE $*
}

add_ost() {
    facet=$1
    shift
    rm -f ${facet}active
    add_facet $facet
    do_lmc --add ost --node ${facet}_facet --ost ${facet}_svc --fstype $FSTYPE --backfstype $OST_BACKFSTYPE $*
}

del_ost() {
    facet=$1
    shift
    do_lmc --delete ost --node ${facet}_facet --ost ${facet}_svc $*
}
add_cmobd() {
    facet=$1
    cache_facet=$2
    master_facet=$3
    shift; shift; shift
    do_lmc --add cmobd --node mds1_facet --cmobd ${facet}_svc --cache_obd ${cache_facet} --master_obd ${master_facet}  
}

add_cobd() {
    facet=$1
    client_facet=$2
    master_facet=$3
    shift; shift; shift
    do_lmc --add cobd --node mds1_facet --cmobd ${facet}_svc --cache_obd ${cache_facet} --master_obd ${master_facet}  
}

deactivate_ost() {
    facet=$1
    shift
    do_lmc --deactivate ost --node ${facet}_facet --ost ${facet}_svc $*
}

add_ostfailover() {
    facet=$1
    shift
    add_facet ${facet}failover
    do_lmc --add ost --failover --node ${facet}failover_facet --ost ${facet}_svc --fstype $FSTYPE --backfstype $OST_BACKFSTYPE $*
}

add_lov() {
    lov=$1
    mds_facet=$2
    shift; shift
    do_lmc --add lov --mds ${mds_facet}_svc --lov $lov $*
}

add_lov_to_lmv() {
    lov=$1
    lmv=$2
    shift; shift
    do_lmc --add lov --lmv $lmv --lov $lov $*
}

add_lov_to_cache_master_lmv() {
    lov=$1
    cache_lmv=$2
    master_lmv=$3
    shift; shift; shift
    do_lmc --add lov --cachelmv $cache_lmv --masterlmv $master_lmv --lov $lov $*
}

add_lmv() {
    lmv=$1
    shift;
    do_lmc --add lmv --lmv $lmv $*
}

add_client() {
    facet=$1
    mds=$2
    shift; shift
    add_facet $facet --lustre_upcall $UPCALL
    do_lmc --add mtpt --node ${facet}_facet --clientoptions async --mds ${mds}_svc $*
}

config_commit() {
    do_lmc --commit
}

####### 
# General functions

check_network() {
   local NETWORK=0
   local WAIT=0
   local MAX=$2
   while [ $NETWORK -eq 0 ]; do
      ping -c 1 -w 3 $1 > /dev/null
      if [ $? -eq 0 ]; then
         NETWORK=1
      else
         WAIT=$((WAIT + 5))
	 echo "waiting for $1, $((MAX - WAIT)) secs left"
         sleep 5
      fi
      if [ $WAIT -gt $MAX ]; then
         echo "Network not available"
         exit 1
      fi
   done
}
check_port() {
   while( !($DSH2 $1 "netstat -tna | grep -q $2") ) ; do
      sleep 9
   done
}

no_dsh() {
   shift
   eval $@
}

comma_list() {
    # the sed converts spaces to commas, but leaves the last space
    # alone, so the line doesn't end with a comma.
    echo "$*" | tr -s " " "\n" | sort -b -u | tr "\n" " " | sed 's/ \([^$]\)/,\1/g'
}

absolute_path() {
   (cd `dirname $1`; echo $PWD/`basename $1`)
}

##################################
# OBD_FAIL funcs

drop_request() {
# OBD_FAIL_MDS_ALL_REQUEST_NET
    RC=0
    do_facet mds "echo 0x123 > /proc/sys/lustre/fail_loc"
    do_facet client "$1" || RC=$?
    do_facet mds "echo 0 > /proc/sys/lustre/fail_loc"
    return $RC
}

drop_reply() {
# OBD_FAIL_MDS_ALL_REPLY_NET
    RC=0
    do_facet mds "echo 0x122 > /proc/sys/lustre/fail_loc"
    do_facet client "$@" || RC=$?
    do_facet mds "echo 0 > /proc/sys/lustre/fail_loc"
    return $RC
}

drop_reint_reply() {
# OBD_FAIL_MDS_REINT_NET_REP
    RC=0
    do_facet mds "echo 0x119 > /proc/sys/lustre/fail_loc"
    do_facet client "$@" || RC=$?
    do_facet mds "echo 0 > /proc/sys/lustre/fail_loc"
    return $RC
}

pause_bulk() {
#define OBD_FAIL_OST_BRW_PAUSE_BULK      0x214
    RC=0
    do_facet ost "echo 0x214 > /proc/sys/lustre/fail_loc"
    do_facet client "$1" || RC=$?
    do_facet client "sync"
    do_facet ost "echo 0 > /proc/sys/lustre/fail_loc"
    return $RC
}

drop_ldlm_cancel() {
#define OBD_FAIL_LDLM_CANCEL             0x304
    RC=0
    do_facet client "echo 0x304 > /proc/sys/lustre/fail_loc"
    do_facet client "$@" || RC=$?
    do_facet client "echo 0 > /proc/sys/lustre/fail_loc"
    return $RC
}

drop_bl_callback() {
#define OBD_FAIL_LDLM_BL_CALLBACK        0x305
    RC=0
    do_facet client "echo 0x305 > /proc/sys/lustre/fail_loc"
    do_facet client "$@" || RC=$?
    do_facet client "echo 0 > /proc/sys/lustre/fail_loc"
    return $RC
}

clear_failloc() {
    facet=$1
    pause=$2
    sleep $pause
    echo "clearing fail_loc on $facet"
    do_facet $facet "sysctl -w lustre.fail_loc=0"
}

cancel_lru_locks() {
    $LCTL mark cancel_lru_locks
    for d in /proc/fs/lustre/ldlm/namespaces/$1*; do
	if [ -f $d/lru_size ]; then
	    echo clear > $d/lru_size
	    grep [0-9] $d/lock_unused_count
	fi
    done
}


pgcache_empty() {
    for a in /proc/fs/lustre/llite/*/dump_page_cache; do
        if [ `wc -l $a | awk '{print $1}'` -gt 1 ]; then
                echo there is still data in page cache $a ?
                cat $a;
                return 1;
        fi
    done
    return 0
}

##################################
# Test interface 
error() {
    echo "${TESTSUITE}: **** FAIL:" $@
    log "FAIL: $@"
    exit 1
}

build_test_filter() {
        [ "$ONLY" ] && log "only running $ONLY"
        for O in $ONLY; do
            eval ONLY_${O}=true
        done
        [ "$EXCEPT$ALWAYS_EXCEPT" ] && log "skipping $EXCEPT $ALWAYS_EXCEPT"
        for E in $EXCEPT $ALWAYS_EXCEPT; do
            eval EXCEPT_${E}=true
        done
}

_basetest() {
    echo $*
}

basetest() {
    IFS=abcdefghijklmnopqrstuvwxyz _basetest $1
}

run_test() {
        export base=`basetest $1`
        if [ ! -z "$ONLY" ]; then
                 testname=ONLY_$1
                 if [ ${!testname}x != x ]; then
                     run_one $1 "$2"
                     return $?
                 fi
                 testname=ONLY_$base
                 if [ ${!testname}x != x ]; then
                     run_one $1 "$2"
                     return $?
                 fi
                 echo -n "."
                 return 0
        fi
        testname=EXCEPT_$1
        if [ ${!testname}x != x ]; then
                 echo "skipping excluded test $1"
                 return 0
        fi
        testname=EXCEPT_$base
        if [ ${!testname}x != x ]; then
                 echo "skipping excluded test $1 (base $base)"
                 return 0
        fi
        run_one $1 "$2"

        return $?
}

EQUALS="======================================================================"
equals_msg() {
   msg="$@"

   local suffixlen=$((${#EQUALS} - ${#msg}))
   [ $suffixlen -lt 5 ] && suffixlen=5
   printf '===== %s %.*s\n' "$msg" $suffixlen $EQUALS
}

log() {
	echo "$*"
	lctl mark "$*" 2> /dev/null || true
}

pass() {
	echo PASS $@
}

run_one() {
    testnum=$1
    message=$2
    tfile=f${testnum}
    tdir=d${base}

    # Pretty tests run faster.
    equals_msg $testnum: $message

    BEFORE=`date +%s`
    log "== test $testnum: $message ============ `date +%H:%M:%S` ($BEFORE)"
    test_${testnum} || error "test_$testnum failed with $?"
    pass "($((`date +%s` - $BEFORE))s)"
}

canonical_path() {
   (cd `dirname $1`; echo $PWD/`basename $1`)
}

