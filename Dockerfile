FROM ubuntu:24.04

# Prevent interactive prompts during apt-get
ENV DEBIAN_FRONTEND=noninteractive

# Install core build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    clang \
    clangd \
    clang-format \
    meson \
    ninja-build \
    pkg-config \
    liburing-dev \
    libboost-context-dev \
    libboost-fiber-dev \
    git \
    python3 \
    && rm -rf /var/lib/apt/lists/*

# Set up the working directory
WORKDIR /app

# Keep the container alive so you can exec into it
CMD ["sleep", "infinity"]
