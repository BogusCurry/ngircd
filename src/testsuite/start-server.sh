#!/bin/sh
# ngIRCd Test Suite
# $Id: start-server.sh,v 1.10.4.1 2003/11/07 20:51:11 alex Exp $

[ -z "$srcdir" ] && srcdir=`dirname $0`

echo "      starting server ..."

# remove old logfiles
rm -rf logs *.log

# check weather getpid.sh returns valid PIDs. If not, don't start up the
# test-server, because we won't be able to kill it at the end of the test.
./getpid.sh sh > /dev/null 2>&1
if [ $? -ne 0 ]; then
  echo "      error: getpid.sh FAILED!"
  exit 1
fi

# check if there is a test-server already running
./getpid.sh T-ngircd > /dev/null 2>&1
if [ $? -eq 0 ]; then
  echo "      error: test-server already running!"
  exit 1
fi

# generate MOTD for test-server
echo "This is an ngIRCd Test Server" > ngircd-test.motd

# starting up test-server ...
./T-ngircd -np -f ${srcdir}/ngircd-test.conf > ngircd-test.log 2>&1 &
sleep 1

# validate running test-server
pid=`./getpid.sh T-ngircd`
[ -n "$pid" ] && kill -0 $pid > /dev/null 2>&1 || exit 1

# -eof-
