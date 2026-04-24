#!/bin/bash -xeu
# https://unix.stackexchange.com/a/729974
FTP_SERVER=192.168.88.39
FTP_PORT=1337
FTP_USER=anonymous
FTP_PASS=anonymous
FTP_DESTINATION_DIR=""
upload_file () {
  local file=$1
  ftp -inv "$FTP_SERVER" "$FTP_PORT" <<EOF
user $FTP_USER $FTP_PASS
binary
cd $2
put $file
quit
EOF
}

export -f upload_file

cd build
upload_file pngshot-ssu.suprx ur0:tai/
cd ..
upload_file config.txt ux0:data/pngshot-ssu/