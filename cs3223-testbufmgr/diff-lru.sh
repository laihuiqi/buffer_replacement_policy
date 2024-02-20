#!/usr/bin/env bash


# LRU
# compare difference between your testresults files and provided solution files

policy=lru
for i in {0..9}
do
	echo "############### testcase $i "
	diff soln-${policy}/result-$i.txt testresults-${policy}/result-$i.txt 
	echo
done

