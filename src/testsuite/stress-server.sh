#!/bin/sh
# ngIRCd Test Suite
# $Id: stress-server.sh,v 1.6.4.1 2003/04/22 20:01:23 alex Exp $

[ -z "$srcdir" ] && srcdir=`dirname $0`

[ "$1" -gt 0 ] 2> /dev/null && CLIENTS="$1" || CLIENTS=5

name=`basename $0`
test=`echo ${name} | cut -d '.' -f 1`
mkdir -p logs tests

type expect > /dev/null 2>&1
if [ $? -ne 0 ]; then
  echo "SKIP: ${name} -- \"expect\" not found.";  exit 77
fi
type telnet > /dev/null 2>&1
if [ $? -ne 0 ]; then
  echo "SKIP: ${name} -- \"telnet\" not found.";  exit 77
fi

echo "      stressing server with $CLIENTS clients (be patient!) ..."
no=0
while [ ${no} -lt $CLIENTS ]; do
  cat ${srcdir}/stress-A.e > tests/${no}.e
  echo "send \"nick test${no}\\r\"" >> tests/${no}.e
  cat ${srcdir}/stress-B.e >> tests/${no}.e
  no=`expr ${no} + 1`
done
no=0
while [ ${no} -lt $CLIENTS ]; do
  expect tests/${no}.e > logs/stress-${no}.log 2> /dev/null &
  no=`expr ${no} + 1`
done

touch logs/check-idle.log
while true; do
  expect ${srcdir}/check-idle.e >> logs/check-idle.log
  res=$?
  [ $res -eq 0 ] && exit 0
  [ $res -eq 1 ] && exit 1
  sleep 1
  echo "====================" >> logs/check-idle.log
done

# -eof-
