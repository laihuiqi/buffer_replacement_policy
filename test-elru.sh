#!/usr/bin/env bash

# Script for testing ELRU policy

# IMPORTANT: You must edit the value of PGPORT variable in settings.sh before running this script

policy=elru

source ./settings.sh

pg_ctl stop

pg_ctl start -l ${LOG_FILE} -o "-p ${PGPORT} -B 16"

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


# check that number of shared buffer pages is configured to 16 pages
if ! psql -c "SHOW shared_buffers;" ${DBNAME} | grep -q "128kB" ; then
	echo "ERROR: restart server with 16 buffer pages!"
	exit 1;
fi


# load data into movies relation
psql -f load-data.sql ${DBNAME}


# create test_bufmgr extension if necessary
psql -c "CREATE EXTENSION IF NOT EXISTS test_bufmgr;" ${DBNAME}


# run tests
TEST_DIR=./testresults-${policy}

if [ -d ${TEST_DIR} ]; then
	\rm -f ${TEST_DIR}/*.txt
else
	mkdir -p ${TEST_DIR}
fi

for testno in  {0..9}
do
	resultfile="${TEST_DIR}/result-$testno.txt"
	echo "Running test case ${testno} -> ${resultfile} ..."
	psql -c "SELECT test_bufmgr('movies', $testno);" ${DBNAME} 2> ${resultfile}
done

