#!/usr/bin/env bash

# ELRU
# compare difference between your testresults files and provided solution files

policy=elru
for i in {0..9}
do
	echo "############### testcase $i "
	diff soln-${policy}/result-$i.txt testresults-${policy}/result-$i.txt 
	echo
done

