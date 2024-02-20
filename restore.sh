#!/usr/bin/env bash

# CS3223 Assignment 1 
# Script to restore installation due to severe corruption

source ./settings.sh

pg_ctl stop
\rm -rf  ${DATA_DIR}
mkdir -p ${DATA_DIR}
make clock

# Create a database cluster
${INSTALL_DIR}/bin/initdb -D ${DATA_DIR}

