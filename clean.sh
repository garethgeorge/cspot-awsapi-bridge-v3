#! /bin/bash 

docker kill  $(docker ps -aq)
docker rm $(docker ps -aq)

pkill docker-current 
pkill libmicrohttpd 
rm -rf ./cspot 
make clean 
make 
