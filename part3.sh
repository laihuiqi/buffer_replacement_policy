#!/usr/bin/env bash

# CS3223 Assignment 1 Part 3 - Benchmarking PostgreSQL (with ELRU replacement policy)

# IMPORTANT: You must edit the value of PGPORT variable in settings.sh before running this script

source ./settings.sh

pg_ctl stop

pg_ctl start -l ${LOG_FILE} -o "-p ${PGPORT} -B 8192"

# check that server is running
if ! pg_ctl status > /dev/null; then
	echo "ERROR: postgres server is not running!"
	exit 1;
fi


# if database exists, drop database
if psql -l | grep -q "${DBNAME}"; then
	echo "Dropping database ${DBNAME} ..."
	dropdb "${DBNAME}"
fi

echo "Creating database ${DBNAME} ..."
createdb "${DBNAME}"


# check that number of shared buffer pages is configured to 64MB
if ! psql -c "SHOW shared_buffers;" ${DBNAME} | grep "64MB"; then
	echo "ERROR: restart server using -B 8192!"
	exit 1;
fi


# create benchmark database relations 
NUM_CLIENTS=10
SCALE_FACTOR=4
DURATION=360
pgbench -i -s ${SCALE_FACTOR} --unlogged-tables  ${DBNAME}

# show size of shared buffers
RESULTFILE=part3_result.txt
psql -c "show shared_buffers;" ${DBNAME}  >| ${RESULTFILE}

# reset statistics counters 
psql -c "SELECT pg_stat_reset();" ${DBNAME}

echo >> ${RESULTFILE}
date >> ${RESULTFILE}
echo >> ${RESULTFILE}


# run benchmark  test
pgbench -c ${NUM_CLIENTS} -T ${DURATION} -j ${NUM_CLIENTS} -s ${SCALE_FACTOR} ${DBNAME} >> ${RESULTFILE}

echo >> ${RESULTFILE}
date >> ${RESULTFILE}
echo >> ${RESULTFILE}


# calculate buffer hit ratio
psql -c "SELECT SUM(heap_blks_read) AS heap_read, SUM(heap_blks_hit)  AS heap_hit, SUM(heap_blks_hit) / (SUM(heap_blks_hit) + SUM(heap_blks_read))  AS hit_ratio FROM pg_statio_user_tables;" ${DBNAME} >> ${RESULTFILE}

cat ${RESULTFILE}

