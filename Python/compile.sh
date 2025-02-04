#!/bin/sh

export PYDIR=/usr/include/python2.7

gcc -O3 -Wall -march=pentium4 -shared -I../Db -I${PYDIR} -o wgdb.so wgdbmodule.c ../Db/dbmem.c ../Db/dballoc.c ../Db/dbdata.c ../Db/dblock.c ../Db/dbindex.c ../Db/dblog.c ../Db/dbhash.c  ../Db/dbcompare.c ../Db/dbquery.c ../Db/dbutil.c ../Db/dbmpool.c ../Db/dbjson.c ../Db/dbschema.c ../json/yajl_all.c
