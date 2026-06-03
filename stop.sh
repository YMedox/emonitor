#!/bin/bash
pid=$(pidof emonitor)
echo $pid
kill -SIGINT $pid
delay 5s
