if [ ! -d "../cspot" ]; then
	echo "Please install cspot and its dependencies in the parent folder of this repo if you will be building from source "
	return 
fi

curdir=$(pwd)


echo "Installing OpenSSL"
yum install openssl 

echo "Installing the versions of Python available to lambdas, this might take a while"

if [ ! -x "$(command -v python3.6)"] ; then 
	yum install -y https://centos7.iuscommunity.org/ius-release.rpm;
	yum update ;
	yum install -y python36u python36u-libs python36u-devel python36u-pip ;
else
	echo "\talready installed"
fi 

echo "Installing the unzip command "
if [ ! -x "$(command -v unzip)"] ; then 
	yum install -y unzip 
else
	echo "\talready installed"
fi 


echo "Installing ulfius"
if [ ! -d "3rdparty/ulfios" ]; then
	yum install \
		gnutls-devel \
		systemd-devel \
		libmicrohttpd-devel \
		jansson-devel \
		libcurl-devel -y

	git clone git@github.com:babelouest/ulfius.git ./3rdparty/ulfius
	cd ./3rdparty/ulfius
	ldconfig 
	git submodule init 
	git submodule update -r 

	cd lib/orcania
	make && sudo make install
	cd ../yder
	make && sudo make install
	cd ../..
	make 
	sudo make install
else 
	echo "\talready installed"
fi
