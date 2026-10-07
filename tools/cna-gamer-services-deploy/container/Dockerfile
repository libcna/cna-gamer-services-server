# syntax=docker/dockerfile:1
FROM debian:13-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential ca-certificates ccache cmake ninja-build libboost-dev libssl-dev \
    libsqlite3-dev nlohmann-json3-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /source
COPY . .
RUN --mount=type=cache,target=/root/.cache/ccache \
    cmake -S . -B build-container -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER_LAUNCHER=ccache && \
    cmake --build build-container --target cna-gamer-services-server cna-gamer-services-admin --parallel 6

FROM debian:13-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl libssl3t64 libsqlite3-0 libstdc++6 && rm -rf /var/lib/apt/lists/* && \
    groupadd --system --gid 65532 cna && useradd --system --uid 65532 --gid cna --home /nonexistent cna && \
    install -d -o cna -g cna -m 0700 /var/lib/cna-gamer-services
COPY --from=build /source/build-container/cna-gamer-services-server /usr/local/bin/
COPY --from=build /source/build-container/cna-gamer-services-admin /usr/local/bin/
USER 65532:65532
EXPOSE 47831
VOLUME ["/var/lib/cna-gamer-services"]
ENTRYPOINT ["/usr/local/bin/cna-gamer-services-server"]
CMD ["--database","/var/lib/cna-gamer-services/service.sqlite3","--listen","0.0.0.0","--port","47831","--cert","/run/secrets/certificate.pem","--key","/run/secrets/private-key.pem","--diagnostics-listen","127.0.0.1","--diagnostics-port","47832","--log-format","json"]
