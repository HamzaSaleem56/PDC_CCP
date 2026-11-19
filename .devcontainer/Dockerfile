# Dev container for the PDC_CCP distributed hash cracker.
# Bakes in the OpenMPI + OpenSSL toolchain so GitHub Codespaces can build and
# run the project with zero manual setup. Based on the standard C++ dev image
# (which already ships gcc, make and the usual build tools).
FROM mcr.microsoft.com/devcontainers/cpp:ubuntu-24.04

RUN apt-get update \
    && export DEBIAN_FRONTEND=noninteractive \
    && apt-get install -y --no-install-recommends \
        openmpi-bin \
        libopenmpi-dev \
        libssl-dev \
        python3 \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*
