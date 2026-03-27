FROM debian:bookworm-slim AS builder

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential cmake \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)"

FROM debian:bookworm-slim AS runtime

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

RUN useradd --create-home --uid 10001 appuser

COPY --from=builder /src/build/http_server_cpp /app/http_server_cpp
COPY --from=builder /src/http_server.conf /app/http_server.conf
COPY --from=builder /src/html_docs /app/html_docs

RUN mkdir -p /app/logs \
    && chown -R appuser:appuser /app

USER appuser

EXPOSE 8080

CMD ["./http_server_cpp", "-c", "./http_server.conf"]