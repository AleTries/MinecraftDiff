FROM ubuntu:18.04

RUN apt-get update && \
    apt-get install -y cmake libxml2-dev g++-8 libpng-dev zlib1g-dev  libboost-program-options-dev

COPY . /bedrock-viz

RUN cd /bedrock-viz && \
    mkdir -p build && cd build && \
    export CC=/usr/bin/gcc-8 && \
    export CXX=/usr/bin/g++-8 && \
    cmake .. && \
    make

#COPY --from=builder /usr/local/share/bedrock-viz /usr/local/share/bedrock-viz
#COPY --from=builder /usr/local/bin/bedrock-viz /usr/local/bin/

#ENTRYPOINT ["/usr/bin/sh"]

