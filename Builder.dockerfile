# Use Ubuntu 22.04 as base image with x86_64 platform
FROM --platform=linux/amd64 ubuntu:22.04

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install system dependencies
RUN apt-get update && apt-get install -y \
    python3.11 \
    python3.11-dev \
    python3.11-venv \
    python3.11-distutils \
    build-essential \
    cmake \
    git \
    libssl-dev \
    libpython3.11-dev \
    && rm -rf /var/lib/apt/lists/*

# Create symbolic link for python3
RUN ln -sf /usr/bin/python3.11 /usr/bin/python

# Set working directory
WORKDIR /build

# Copy RApiPlus dependencies first
COPY builder/lib /usr/local/lib
COPY builder/include /usr/local/include
COPY builder/rithmic_ssl_cert_auth_params /usr/local/share/rithmic_ssl_cert_auth_params

# Update shared library cache
RUN ldconfig

# Copy the entire project
COPY . .

# Create and activate virtual environment
RUN python -m venv /opt/venv
ENV PATH="/opt/venv/bin:$PATH"

# Install build dependencies
RUN pip install --upgrade pip setuptools wheel pybind11

# Clean up any existing build artifacts and create fresh build directory
RUN rm -rf build CMakeCache.txt CMakeFiles && \
    mkdir -p build

# Verify Python installation and paths
RUN echo "Python executable: $(which python3.11)" && \
    echo "Python include dir: $(python3.11 -c 'import sysconfig; print(sysconfig.get_path("include"))')" && \
    echo "Python library: $(python3.11 -c 'import sysconfig; print(sysconfig.get_config_var("LIBDIR"))')/libpython3.11.so"

# Build the package with explicit Python paths and verbose output
RUN cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release \
          -DPython3_EXECUTABLE=$(which python3.11) \
          -DPython3_INCLUDE_DIR=$(python3.11 -c 'import sysconfig; print(sysconfig.get_path("include"))') \
          -DPython3_LIBRARY=$(python3.11 -c 'import sysconfig; print(sysconfig.get_config_var("LIBDIR"))')/libpython3.11.so \
          -DCMAKE_LIBRARY_PATH=/usr/local/lib \
          -DCMAKE_INCLUDE_PATH=/usr/local/include \
          .. && \
    VERBOSE=1 make -j1 && \
    echo "Build directory contents:" && \
    ls -la && \
    echo "Looking for rapi.so:" && \
    find . -name "rapi.so"

# Create output directory and copy build artifacts
RUN mkdir -p /build/output && \
    cp build/python/rapi.so /build/output/ && \
    mkdir -p /build/output/lib /build/output/include && \
    cp -r /usr/local/lib/* /build/output/lib/ && \
    cp -r /usr/local/include/* /build/output/include/ && \
    cp /usr/local/share/rithmic_ssl_cert_auth_params /build/output/ && \
    echo "Output directory contents:" && \
    ls -la /build/output && \
    echo "Checking rapi.so:" && \
    ls -la /build/output/rapi.so 