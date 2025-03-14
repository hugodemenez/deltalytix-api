# Use a specific version of gcc that matches your library architecture
FROM --platform=linux/amd64 gcc:9.4.0
# Install required dependencies
RUN apt-get update && apt-get install -y \
    libssl-dev \
    zlib1g-dev \
    build-essential \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /build

# First, copy only the libraries to verify they exist
COPY builder/lib/ /build/lib/

# Verify library files exist and show their architecture
RUN ls -la /build/lib/ && \
    file /build/lib/*.a || true

# Add these commands after copying the libraries to inspect them
RUN file /build/lib/libRApiPlus-optimize.a
RUN gcc -v 2>&1 | grep Target

# Copy remaining files
COPY builder/include/ /build/include/
COPY builder/OrderFetcher.cpp /build/OrderFetcher.cpp
COPY builder/GetAccountList.cpp /build/GetAccountList.cpp
COPY builder/rithmic_ssl_cert_auth_params/ /build/rithmic_ssl_cert_auth_params/

# Common compiler flags
ENV CXXFLAGS="-O3 -DLINUX -D_REENTRANT -Wall -Wno-sign-compare \
    -Wno-write-strings -Wpointer-arith -Winline -Wno-deprecated \
    -fno-strict-aliasing -I./include"

ENV LDFLAGS="-L./lib \
    -lRApiPlus-optimize -lOmneStreamEngine-optimize \
    -lOmneChannel-optimize -lOmneEngine-optimize -l_api-optimize \
    -l_apipoll-stubs-optimize -l_kit-optimize -lssl -lcrypto \
    -L/usr/lib64 -lz -lpthread -lrt -ldl"

# Compile both executables
RUN g++ -std=c++11 $CXXFLAGS -o OrderFetcher ./OrderFetcher.cpp $LDFLAGS && \
    g++ $CXXFLAGS -o GetAccountList ./GetAccountList.cpp $LDFLAGS

# List the files before copying
RUN ls -la OrderFetcher GetAccountList

# Copy both compiled binaries to output
CMD cp OrderFetcher GetAccountList /build/output/ && \
    ls -la /build/output/OrderFetcher /build/output/GetAccountList