# Build stage
FROM --platform=linux/amd64 ubuntu:22.04 AS builder

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

# Copy RApiPlus dependencies and set up proper permissions
RUN mkdir -p /usr/local/lib /usr/local/include
COPY builder/lib/* /usr/local/lib/
COPY builder/include/* /usr/local/include/
COPY builder/rithmic_ssl_cert_auth_params /usr/local/share/rithmic_ssl_cert_auth_params

# Set proper permissions and update library cache
RUN chmod 644 /usr/local/lib/* && \
    chmod 644 /usr/local/include/* && \
    chmod 644 /usr/local/share/rithmic_ssl_cert_auth_params && \
    echo "/usr/local/lib" > /etc/ld.so.conf.d/local.conf && \
    ldconfig

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

# Build the package with explicit Python paths and verbose output
RUN cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release \
          -DPython3_EXECUTABLE=$(which python3.11) \
          -DPython3_INCLUDE_DIR=$(python3.11 -c 'import sysconfig; print(sysconfig.get_path("include"))') \
          -DPython3_LIBRARY=$(python3.11 -c 'import sysconfig; print(sysconfig.get_config_var("LIBDIR"))')/libpython3.11.so \
          -DCMAKE_LIBRARY_PATH=/usr/local/lib \
          -DCMAKE_INCLUDE_PATH=/usr/local/include \
          .. && \
    VERBOSE=1 make -j1

# Verify the build artifacts and debug library info
RUN ls -la python/rapi.so || exit 1 && \
    echo "=== Library dependencies ===" && \
    ldd python/rapi.so && \
    echo "=== Library search paths ===" && \
    ldconfig -v && \
    echo "=== Library contents ===" && \
    ls -la /usr/local/lib/

# Create output directory and copy build artifacts with debug info
RUN mkdir -p /build/output/lib /build/output/include && \
    # Copy the built Python module
    cp python/rapi.so /build/output/ && \
    # Copy library files, excluding Python directories
    find /usr/local/lib -maxdepth 1 -type f -exec cp -P {} /build/output/lib/ \; && \
    # Copy include files
    find /usr/local/include -type f -exec cp -P {} /build/output/include/ \; && \
    # Copy SSL cert
    cp /usr/local/share/rithmic_ssl_cert_auth_params /build/output/ && \
    # Set permissions
    find /build/output/lib -type f -exec chmod 644 {} \; && \
    find /build/output/include -type f -exec chmod 644 {} \; && \
    chmod 644 /build/output/rithmic_ssl_cert_auth_params && \
    echo "=== Output library contents ===" && \
    ls -la /build/output/lib/

# Final stage
FROM --platform=linux/amd64 ubuntu:22.04

# Install system dependencies
RUN apt-get update && apt-get install -y \
    python3.11 \
    python3.11-dev \
    python3.11-venv \
    python3.11-distutils \
    libzmq3-dev \
    curl \
    libstdc++6 \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Create symbolic link for python3
RUN ln -sf /usr/bin/python3.11 /usr/bin/python

WORKDIR /app

# Create and activate virtual environment
RUN python -m venv /opt/venv
ENV PATH="/opt/venv/bin:$PATH"

# Copy requirements and install Python dependencies
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Create necessary directories with proper permissions
RUN mkdir -p /app/logs /app/temp_db /app/bin && \
    chmod 755 /app/logs /app/temp_db /app/bin

# Copy application code
COPY app app/

# Copy configuration files
COPY server_configurations.json ./
COPY server_configurations.json /app/bin/
COPY server_configurations.json /app/app/

# Create lib directory in final stage and set permissions
RUN mkdir -p /app/bin/lib /app/bin/include && \
    chmod 755 /app/bin/lib /app/bin/include && \
    echo "/app/bin/lib" > /etc/ld.so.conf.d/app.conf && \
    ldconfig

# Copy built rapi package and dependencies from builder
COPY --from=builder /build/output/rapi.so /app/rapi.so
COPY --from=builder /build/output/lib/* /app/bin/lib/
COPY --from=builder /build/output/include/* /app/bin/include/
COPY --from=builder /build/output/rithmic_ssl_cert_auth_params /app/bin/rithmic_ssl_cert_auth_params

# Set proper permissions and update library cache again
RUN chmod 644 /app/rapi.so && \
    find /app/bin/lib -type f -exec chmod 644 {} \; && \
    find /app/bin/include -type f -exec chmod 644 {} \; && \
    chmod 644 /app/bin/rithmic_ssl_cert_auth_params && \
    chmod 644 /app/bin/server_configurations.json && \
    chmod 644 /app/server_configurations.json && \
    chmod 644 /app/app/server_configurations.json && \
    ldconfig && \
    echo "=== Final library setup ===" && \
    echo "Library search paths:" && \
    ldconfig -v 2>/dev/null | grep -v "^\\s" && \
    echo "Library dependencies:" && \
    ldd /app/rapi.so && \
    echo "Library contents:" && \
    ls -la /app/bin/lib/

# Set environment variables
ENV PYTHONPATH=/app
ENV LOG_LEVEL=INFO
ENV LOG_FILE=/app/logs/app.log
ENV PYTHONDONTWRITEBYTECODE=1
ENV PYTHONUNBUFFERED=1
ENV PATH="/app/bin:${PATH}"
ENV LD_LIBRARY_PATH="/app/bin/lib:${LD_LIBRARY_PATH}"

# Run the application
CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000", "--reload", "--log-level", "debug", "--access-log", "--log-config", "app/logging_config.json", "--ws", "websockets", "--ws-max-size", "16777216", "--ws-max-queue", "32", "--ws-ping-interval", "20", "--ws-ping-timeout", "20", "--limit-concurrency", "1000", "--backlog", "2048"]