if [ ! -d "../cspot" ]; then
	echo "Please install cspot and its dependencies in the parent folder of this repo if you will be building from source "
	return 
fi

curdir=$(pwd)


# echo "Installing latest gcc and g++"
# sudo yum install centos-release-scl
# sudo yum install devtoolset-7-gcc
# sudo yum install devtoolset-7-gcc-c++
# scl enable devtoolset-7 bash

# echo "Install Boost"
yum install boost-devel

echo "Installing OpenSSL"
yum install openssl openssl-devel 

echo "Installing the versions of Python available to lambdas, this might take a while"

if [ ! -x "$(command -v python3.6)"] ; then 
	yum install -y https://centos7.iuscommunity.org/ius-release.rpm;
	yum update ;
	yum install -y python36u python36u-libs python36u-devel python36u-pip ;
else
	echo "\talready installed"
fi 

echo "Installing the unzip command "
yum install -y unzip 

echo "Installing ulfius"
if [ ! -d "3rdparty/ulfius" ]; then
	yum install \
		gnutls-devel \
		systemd-devel \
		libmicrohttpd-devel \
		jansson-devel \
		libcurl-devel -y
	
	mkdir -p ./3rdparty/ulfius 
	cd ./3rdparty/ulfius

	git clone https://github.com/babelouest/orcania.git
	git clone https://github.com/babelouest/yder.git
	git clone https://github.com/babelouest/ulfius.git
	cd orcania/
	make && sudo make install
	cd ../yder/
	make && sudo make install
	cd ../ulfius/src/
	make && sudo make install
	
	cd $curdir
else 
	echo "\talready installed"
fi
