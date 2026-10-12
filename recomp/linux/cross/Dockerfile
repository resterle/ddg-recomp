# x86-64 cross compiler for the aarch64 build (linux/build-cross.sh): clang 16 and lld 16, the
# versions build.sh uses natively (so linux/perfect.profdata stays readable).
FROM --platform=linux/amd64 debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl gnupg make zip && \
    curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor -o /usr/share/keyrings/llvm.gpg && \
    echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] https://apt.llvm.org/bookworm/ llvm-toolchain-bookworm-16 main" \
        > /etc/apt/sources.list.d/llvm.list && \
    apt-get update && apt-get install -y --no-install-recommends clang-16 lld-16 llvm-16 && \
    rm -rf /var/lib/apt/lists/*
