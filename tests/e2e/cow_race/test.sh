#!/bin/sh
gcc -pthread cow_race.c -o test_cow_race
./test_cow_race
