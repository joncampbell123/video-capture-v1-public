#!/bin/bash
N=`basename \`pwd\``
cd ..
tar -cvzf package.tar.gz $N/{*.png,videocap,capture_v4l,README}
