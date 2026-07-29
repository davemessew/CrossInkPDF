FROM ubuntu:24.04@sha256:4fbb8e6a8395de5a7550b33509421a2bafbc0aab6c06ba2cef9ebffbc7092d90

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        build-essential \
        ca-certificates \
        curl \
        fontconfig \
        fonts-dejavu-core \
        fonts-liberation \
        git \
        libssl-dev \
        libsdl2-dev \
        python3 \
        python3-venv \
    && curl --fail --location --silent --show-error \
        https://apt.llvm.org/llvm-snapshot.gpg.key \
        --output /usr/share/keyrings/llvm-snapshot.gpg.key \
    && echo \
        "deb [signed-by=/usr/share/keyrings/llvm-snapshot.gpg.key] https://apt.llvm.org/noble/ llvm-toolchain-noble-21 main" \
        > /etc/apt/sources.list.d/llvm-21.list \
    && apt-get update \
    && apt-get install --yes --no-install-recommends clang-format-21 \
    && rm -rf /var/lib/apt/lists/*

RUN python3 -m venv /opt/crossink-python \
    && /opt/crossink-python/bin/pip install --no-cache-dir \
        https://github.com/pioarduino/platformio-core/archive/refs/tags/v6.1.19.zip

COPY docker/pdf-simulator/self-test.sh /usr/local/bin/crossink-simulator-self-test
RUN chmod 0755 /usr/local/bin/crossink-simulator-self-test \
    && mkdir -p /home/ubuntu/.platformio \
    && chown -R ubuntu:ubuntu /home/ubuntu/.platformio

ENV PATH="/opt/crossink-python/bin:${PATH}"
ENV PLATFORMIO_CORE_DIR="/home/ubuntu/.platformio"

USER ubuntu
WORKDIR /workspace
