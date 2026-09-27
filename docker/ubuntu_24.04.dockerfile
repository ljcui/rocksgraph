FROM ubuntu:24.04

ARG JFLAG=-j8

RUN apt update && apt install -y build-essential cmake python3-pip gdb git vim curl ca-certificates libgtest-dev libgflags-dev default-jdk libboost-all-dev libjemalloc-dev openssh-server libzstd-dev liblz4-dev libsnappy-dev libspdlog-dev locales openssh-server clang-format clang-tidy cmake-format protobuf-compiler libprotobuf-dev

RUN wget https://github.com/facebook/rocksdb/archive/refs/tags/v11.8.1.tar.gz -O /tmp/rocksdb.tar.gz && \
    cd /tmp && mkdir rocksdb && tar -xzf rocksdb.tar.gz --strip-components=1 -C rocksdb && cd rocksdb && \
    mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DWITH_GFLAGS=1 \
    -DWITH_JEMALLOC=ON \
    -DWITH_LZ4=ON \
    -DWITH_ZSTD=ON \
    -DWITH_TOOLS=OFF \
    -DWITH_CORE_TOOLS=OFF \
    -DWITH_TRACE_TOOLS=OFF \
    -DWITH_BENCHMARK_TOOLS=OFF \
    -DPORTABLE=ON .. \
    && make ${JFLAG} && make install && \
    cd / && rm -rf /tmp/rocksdb*

RUN wget https://github.com/antlr/antlr4/archive/refs/tags/4.13.2.tar.gz -O /tmp/antlr4.tar.gz && \
    cd /tmp && mkdir antlr4 && tar -xzf antlr4.tar.gz --strip-components=1 -C antlr4 && cd antlr4/runtime/Cpp && \
    mkdir build && cd build && cmake -DWITH_DEMO=0 -DANTLR_BUILD_CPP_TESTS=0 -DANTLR4_INSTALL=1 -DCMAKE_CXX_STANDARD=20 .. && \
    make ${JFLAG} && make install && \
    cd / && rm -rf /tmp/antlr4*

RUN wget https://github.com/OpenMathLib/OpenBLAS/archive/refs/tags/v0.3.28.tar.gz -O /tmp/openblas.tar.gz && \
    cd /tmp && mkdir openblas && tar -xzf openblas.tar.gz --strip-components=1 -C openblas && cd openblas && \
    make ${JFLAG} && make install PREFIX=/usr/local && \
    cd / && rm -rf /tmp/openblas*

RUN wget https://github.com/facebookresearch/faiss/archive/refs/tags/v1.9.0.tar.gz -O /tmp/faiss.tar.gz && \
    cd /tmp && mkdir faiss && tar -xzf faiss.tar.gz --strip-components=1 -C faiss && cd faiss && \
    mkdir build && cd build && cmake -DFAISS_ENABLE_GPU=OFF -DFAISS_ENABLE_PYTHON=OFF -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release -DFAISS_OPT_LEVEL=generic .. && \
    make ${JFLAG} && make install && \
    cd / && rm -rf /tmp/faiss*

RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y

RUN python3 -m pip install --no-cache-dir --break-system-packages \
    "behave>=1.3.3,<2" "neo4j>=5.0,<7"

RUN mkdir -p /usr/local/share/antlr4 && \
    wget https://www.antlr.org/download/antlr-4.13.2-complete.jar \
    -O /usr/local/share/antlr4/antlr-4.13.2-complete.jar
