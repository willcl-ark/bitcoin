ARG host="x86_64-linux-musl"
FROM quay.io/stagex/pallet-clang-cmake-busybox@sha256:f8fdcb0af4db599630c55b67fc98eb6f7195b9838fe110280f7c7dd1e497015c AS depends
COPY --from=quay.io/stagex/core-curl@sha256:a4a9f7159e05508c62e22fd566e9be440b3dd5817c2a97c250e727aa787325aa . /
COPY --from=quay.io/stagex/core-make@sha256:a47d8f67f8fd74905f724fdc5f0d18e29df96391751754947bdeaba9bef8f7d8 . /
COPY --from=quay.io/stagex/user-patch@sha256:b6df2c5a1cec26dca1a689913daa6fb0c53b8c1accacbd6dfc29a07cc4fc2a98 . /
COPY --from=quay.io/stagex/core-m4@sha256:5d4cf0204b121e4d8e76e2575cb1d2b372487f184927118fcebcaa58a1d4e353 . /
COPY --from=quay.io/stagex/core-bash@sha256:7923fb9c4887bfce765a1019febb805d90ab899b8343ba64c63e91442d73f3b1 . /
COPY --from=quay.io/stagex/core-ca-certificates@sha256:85780a2557bae5c7ed1c7af3d20d66c77e10284da3e538b90755e18cc5ffbe33 . /
COPY --from=quay.io/stagex/user-ninja@sha256:686b6d28f8daa88e4ac32b937b28b5bb2ce222396000bf440824fda29f2482e5 . /
COPY --from=quay.io/stagex/user-doxygen@sha256:c40a1cc726536ac3792e9908c75490f3adba8148c7198759d3f0f6893567290b . /
# For QT
# COPY --from=quay.io/stagex/core-bison@sha256:aaabf3166b5ec5e3a6f5b15602d3b97aab1ae22e1f911526c600b5c3e431a143 . /
# COPY --from=quay.io/stagex/user-libx11@sha256:891bdf830af036d48ad39ba58f8dacd5675a45483ba37f1564a1ce4395e2b100 . /
# Think we are missing libxkb needed or something
# COPY --from=quay.io/stagex/core-xz@sha256:ddbb029f22e3b72aae99823cb4869a1319925888cd71a22fe1948beb9219c226 . /
COPY . /bitcoin
ARG host
RUN HOST="${host}" make -C /bitcoin/depends -j `nproc` NO_QT=1 \
        x86_64_linux_musl_CC=clang \
        x86_64_linux_musl_CXX=clang++ \
        x86_64_linux_musl_AR=llvm-ar \
        x86_64_linux_musl_RANLIB=llvm-ranlib \
        x86_64_linux_musl_NM=llvm-nm \
        x86_64_linux_musl_STRIP=llvm-strip

FROM depends AS build
ARG host
ARG SOURCE_DATE_EPOCH
ENV TZ=UTC
ENV SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH}

ENV CFLAGS="-O2 -g -fdebug-prefix-map=/bitcoin=. -fmacro-prefix-map=/bitcoin=."
ENV CXXFLAGS="-O2 -g -fdebug-prefix-map=/bitcoin=. -fmacro-prefix-map=/bitcoin=."
ENV LDFLAGS="-static"

RUN umask 0022

RUN env CFLAGS="${CFLAGS}" CXXFLAGS="${CXXFLAGS}" LDFLAGS="${LDFLAGS}" \
    cmake -S /bitcoin -B /build --toolchain /bitcoin/depends/${host}/toolchain.cmake \
          -DREDUCE_EXPORTS=ON \
          -DBUILD_BENCH=OFF \
          -DBUILD_GUI_TESTS=OFF \
          -DBUILD_FUZZ_BINARY=OFF \
          -DCMAKE_SKIP_RPATH=TRUE

RUN cmake --build /build --parallel
RUN cmake --install /build --prefix /opt/bitcoin

FROM scratch AS package
COPY --from=build /opt/bitcoin /opt/bitcoin
