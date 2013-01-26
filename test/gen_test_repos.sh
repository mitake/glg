#! /bin/bash

mkdir test.git

cd test.git

# make inital commit

git init .
touch first
git add first
python ../gen_big_file.py `pwd`'/'big
git add big
git commit -m "initial commit"

for i in `seq -w 0 29`;
do
    cp big $i
    git add $i
    git commit -m "commit $i"
done

