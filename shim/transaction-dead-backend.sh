#!/bin/sh
#
# transaction-dead-backend.sh — prints the xid of a backend killed mid-transaction.
#
# Opens a session that writes and then sleeps inside its transaction, waits until
# the server reports its backend_xid, terminates the backend, and prints the xid.
# The caller then asks transaction_status about it: a backend that died without
# committing must read as aborted. Reads the port and socket from KWABI_PORT and
# KWABI_PSOCK, which ci-local.sh sets.

PSQL="${KWABI_PSQL:-psql}"
COMMON="-h $KWABI_PSOCK -p $KWABI_PORT -At -q postgres"

$PSQL $COMMON -c "BEGIN; INSERT INTO kwabi_dead_t VALUES (1); SELECT pg_sleep(60);" >/dev/null 2>&1 &

pid=""
i=0
while [ -z "$pid" ] && [ "$i" -lt 100 ]; do
    sleep 0.1
    pid=$($PSQL $COMMON -c "SELECT pid FROM pg_stat_activity WHERE query LIKE 'BEGIN; INSERT INTO kwabi_dead_t%' AND pid <> pg_backend_pid()")
    i=$((i + 1))
done

xid=""
i=0
while [ -z "$xid" ] && [ "$i" -lt 100 ]; do
    sleep 0.1
    xid=$($PSQL $COMMON -c "SELECT backend_xid FROM pg_stat_activity WHERE pid = $pid")
    i=$((i + 1))
done

$PSQL $COMMON -c "SELECT pg_terminate_backend($pid)" >/dev/null 2>&1
sleep 0.5
echo "$xid"
