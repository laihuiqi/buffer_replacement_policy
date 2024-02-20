#!/bin/bash

PORT_NUM=5111
DATA_DIR=$HOME/pgdata-cs3223
INSTALL_DIR=$HOME/pgsql-cs3223
LOG_FILE=$HOME/log.txt
DBNAME=assign1

BASH_PROFILE=$HOME/.bash_profile
touch ${BASH_PROFILE} 
cat <<EOF >> ${BASH_PROFILE}

export PATH=${INSTALL_DIR}/bin:$PATH
export MANPATH=${INSTALL_DIR}/share/man:$MANPATH
export PGDATA=${DATA_DIR}
export PGUSER=$(whoami)
export PGPORT=${PORT_NUM}
EOF
source ${BASH_PROFILE}
