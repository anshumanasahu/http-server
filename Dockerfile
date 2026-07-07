FROM ubuntu:22.04

# Avoid prompts during apt installs
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libssl-dev \
    zlib1g-dev \
    gdb \
    && rm -rf /var/lib/apt/lists/*

# Set up the working directory
WORKDIR /app

# Copy the source code
COPY . .

# Build the project
RUN mkdir -p build && cd build && cmake .. && make

# Expose ports (8080 for HTTP, 8443 for HTTPS in the future)
EXPOSE 8080 8443

# Run the server
CMD ["./build/http-server"]
