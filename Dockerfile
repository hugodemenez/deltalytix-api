FROM --platform=linux/amd64 python:3.9-slim

# Install system dependencies
RUN apt-get update && apt-get install -y \
    libzmq3-dev \
    curl \
    libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy requirements and install Python dependencies
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Create necessary directories
RUN mkdir -p /app/logs /app/temp_db /app/bin

# Copy application code
COPY app app/

# Copy configuration files
COPY server_configurations.json ./
COPY server_configurations.json /app/bin/
COPY server_configurations.json /app/app/

# Copy and set up executables and dependencies
COPY builder/lib /app/bin/lib/
COPY builder/include /app/bin/include/
COPY builder/rithmic_ssl_cert_auth_params /app/bin/
COPY build/OrderFetcher build/GetAccountList /app/bin/

# Set proper permissions and create symlinks
RUN chmod +x /app/bin/OrderFetcher /app/bin/GetAccountList && \
    chmod -R 755 /app/bin/lib/ && \
    chmod -R 755 /app/bin/include/ && \
    chmod 644 /app/bin/rithmic_ssl_cert_auth_params && \
    chmod 644 /app/bin/server_configurations.json && \
    chmod 644 /app/server_configurations.json && \
    chmod 644 /app/app/server_configurations.json && \
    ln -s /app/bin/OrderFetcher /app/OrderFetcher && \
    ln -s /app/bin/GetAccountList /app/GetAccountList

# Set environment variables
ENV PYTHONPATH=/app
ENV LOG_LEVEL=INFO
ENV LOG_FILE=/app/logs/app.log
ENV PYTHONDONTWRITEBYTECODE=1
ENV PYTHONUNBUFFERED=1
ENV PATH="/app/bin:${PATH}"
ENV LD_LIBRARY_PATH="/app/bin/lib:${LD_LIBRARY_PATH}"

# Run the application
CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000", "--reload", "--log-level", "debug", "--access-log", "--ws", "websockets", "--ws-max-size", "16777216", "--ws-max-queue", "32", "--ws-ping-interval", "20", "--ws-ping-timeout", "20", "--limit-concurrency", "1000", "--backlog", "2048"]