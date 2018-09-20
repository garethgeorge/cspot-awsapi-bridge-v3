#! /bin/bash 

cd cspot 

# pkill woofc-namespace 

./lambda_client
# ./s3_client

# trap 'kill $(jobs -p)' EXIT
# sleep 1000000

pkill woofc-namespace 
