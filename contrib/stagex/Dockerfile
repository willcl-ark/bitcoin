ARG host="x86_64-linux-musl"
FROM quay.io/stagex/pallet-clang-cmake-busybox@sha256:f8fdcb0af4db599630c55b67fc98eb6f7195b9838fe110280f7c7dd1e497015c AS depends
# COPY --from=quay.io/stagex/core-lld@sha256:565a1cf8fd567944354f01f07970afbee097e659acf749d871fa6ddd98e6b11f . /
COPY --from=quay.io/stagex/core-curl@sha256:a4a9f7159e05508c62e22fd566e9be440b3dd5817c2a97c250e727aa787325aa . /
COPY --from=quay.io/stagex/core-make@sha256:a47d8f67f8fd74905f724fdc5f0d18e29df96391751754947bdeaba9bef8f7d8 . /
COPY --from=quay.io/stagex/user-patch@sha256:b6df2c5a1cec26dca1a689913daa6fb0c53b8c1accacbd6dfc29a07cc4fc2a98 . /
COPY --from=quay.io/stagex/core-m4@sha256:5d4cf0204b121e4d8e76e2575cb1d2b372487f184927118fcebcaa58a1d4e353 . /
COPY --from=quay.io/stagex/core-bash@sha256:7923fb9c4887bfce765a1019febb805d90ab899b8343ba64c63e91442d73f3b1 . /
COPY --from=quay.io/stagex/core-ca-certificates@sha256:85780a2557bae5c7ed1c7af3d20d66c77e10284da3e538b90755e18cc5ffbe33 . /
COPY --from=quay.io/stagex/user-ninja@sha256:686b6d28f8daa88e4ac32b937b28b5bb2ce222396000bf440824fda29f2482e5 . /
COPY --from=quay.io/stagex/core-tar@sha256:a28dbc52123feba5120a66e86a795061ab55c1c74dffe0be769cd3cc6861d5c5 . /
# COPY --from=quay.io/stagex/user-doxygen@sha256:c40a1cc726536ac3792e9908c75490f3adba8148c7198759d3f0f6893567290b . /
# COPY --from=quay.io/stagex/core-bison@sha256:aaabf3166b5ec5e3a6f5b15602d3b97aab1ae22e1f911526c600b5c3e431a143 . /
# COPY --from=quay.io/stagex/user-libx11@sha256:891bdf830af036d48ad39ba58f8dacd5675a45483ba37f1564a1ce4395e2b100 . /
# COPY --from=quay.io/stagex/core-xz@sha256:ddbb029f22e3b72aae99823cb4869a1319925888cd71a22fe1948beb9219c226 . /
# Think we are missing libxkb needed or something

COPY . /bitcoin

# Build depends
ARG host
RUN --mount=type=cache,target=/bitcoin/depends/sources,id=bitcoin-sources \
    --mount=type=cache,target=/bitcoin/depends/built,id=bitcoin-built \
    HOST="${host}" make -C /bitcoin/depends -j `nproc` NO_QT=1 \
        SOURCES_PATH=/bitcoin/depends/sources \
        BASE_CACHE=/bitcoin/depends/built \
        ${host}_CC=clang \
        ${host}_CXX=clang++ \
        ${host}_AR=llvm-ar \
        ${host}_RANLIB=llvm-ranlib \
        ${host}_NM=llvm-nm \
        ${host}_STRIP=llvm-strip \
        LDFLAGS="${LDFLAGS}"

FROM depends AS build
ARG host
ARG SOURCE_DATE_EPOCH
ARG CFLAGS
ARG CXXFLAGS
ARG LDFLAGS
ARG CMAKE_POSITION_INDEPENDENT_CODE
ENV TZ=UTC
ENV SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH}
ENV CFLAGS=${CFLAGS}
ENV CXXFLAGS=${CXXFLAGS}
ENV LDFLAGS=${LDFLAGS}

RUN umask 0022

RUN --mount=type=cache,target=/bitcoin/depends/built,id=bitcoin-built-${host} \
    env CFLAGS="${CFLAGS}" CXXFLAGS="${CXXFLAGS}" LDFLAGS="${LDFLAGS}" \
    cmake -S /bitcoin -B /build --toolchain /bitcoin/depends/${host}/toolchain.cmake \
          -DREDUCE_EXPORTS=ON \
          -DBUILD_BENCH=OFF \
          -DBUILD_GUI_TESTS=OFF \
          -DBUILD_FUZZ_BINARY=OFF \
          -DCMAKE_SKIP_RPATH=TRUE \
          -DCMAKE_POSITION_INDEPENDENT_CODE=${CMAKE_POSITION_INDEPENDENT_CODE}

RUN --mount=type=cache,target=/bitcoin/depends/built,id=bitcoin-built-${host} \
    cmake --build /build --parallel
RUN cmake --install /build --prefix /opt/bitcoin

# Create distribution directory and split debug symbols
ARG GIT_REVISION
ARG SOURCE_DATE_EPOCH
RUN mkdir -p /opt/dist
RUN cd /opt && \
    cp -r bitcoin dist/bitcoin-${GIT_REVISION} && \
    cd dist/bitcoin-${GIT_REVISION} && \
    \
    # Split debug symbols for binaries using LLVM tools directly \
    find bin -type f -executable | while read -r binary; do \
        llvm-objcopy --enable-deterministic-archives -p --only-keep-debug "$binary" "$binary.dbg" && \
        llvm-objcopy --enable-deterministic-archives -p --strip-debug "$binary" "$binary" && \
        llvm-strip --enable-deterministic-archives -p -s "$binary" && \
        llvm-objcopy --enable-deterministic-archives -p --add-gnu-debuglink="$binary.dbg" "$binary"; \
    done && \
    \
    # Set deterministic timestamps on all files \
    find . -print0 | xargs -0r touch --no-dereference --date="@${SOURCE_DATE_EPOCH}" && \
    \
    # Create tarballs \
    find . -not -name "*.dbg" -print0 | sort -z | \
        tar --create --no-recursion --mode='u+rw,go+r-w,a+X' --null --files-from=- | \
        gzip -9n > /opt/dist/bitcoin-${GIT_REVISION}-${host}.tar.gz && \
    \
    find . -name "*.dbg" -print0 | sort -z | \
        tar --create --no-recursion --mode='u+rw,go+r-w,a+X' --null --files-from=- | \
        gzip -9n > /opt/dist/bitcoin-${GIT_REVISION}-${host}-debug.tar.gz

FROM scratch AS package
COPY --from=build /opt/bitcoin /opt/bitcoin
COPY --from=build /opt/dist /opt/dist
