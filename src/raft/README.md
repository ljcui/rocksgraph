## Introduction

This project is a c++ port of etcd raft, based on a commit point in the original repository (https://github.com/etcd-io/raft), the commit id is in the `commit` file.

## Features

* Almost line-by-line translation of the etcd golang raft code.
* Header-only, all the code is in the header file and does not need to be compiled into a lib.
* Most of the test case code was translated and the test passed.