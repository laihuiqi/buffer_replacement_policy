#!/usr/bin/env bash

# copy new files and install test_bufmgr extension

SRC_DIR=${HOME}/postgresql-16.1
ASSIGN_DIR=${HOME}/cs3223-assign1
chmod u+x *.sh
cp -f Makefile diff-lru.sh diff-elru.sh ${ASSIGN_DIR}
cp -fr soln-lru ${ASSIGN_DIR}
cp -fr soln-elru ${ASSIGN_DIR}
cp -f test_bufmgr.c ${ASSIGN_DIR}/test_bufmgr
\rm -fr ${ASSIGN_DIR}/test_bufmgr/testcases
cp -r testcases ${ASSIGN_DIR}/test_bufmgr
\rm -rf ${SRC_DIR}/contrib/test_bufmgr
cp -r ${ASSIGN_DIR}/test_bufmgr ${SRC_DIR}/contrib/
cd ${SRC_DIR}/contrib/test_bufmgr
make && make install

