#!/bin/bash
#
# Extract the root certificate from octopus.energy

#set -x

openssl s_client -showcerts -connect octopus.energy:443 </dev/null >hoge

start=`grep -e "-----BEGIN CERTIFICATE-----" -n hoge | sed -e 's/:.*//g' | tail -n 1`

last=`grep -e "-----END CERTIFICATE-----" -n hoge | sed -e 's/:.*//g' | tail -n 1`

sed -n ${start},${last}p hoge > main/octopus_energy_root_cert.pem

rm hoge
