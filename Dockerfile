# syntax=docker/dockerfile:1
#
# Two stages: a build stage with the compiler and headers, and a small
# runtime stage with only the binary, the static files and libsqlite3.
#
#   docker build -t http-server .
#   docker run --rm -p 8080:8080 http-server

# ---- build ------------------------------------------------------------------
FROM ubuntu:24.04 AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ make cmake libsqlite3-dev pkg-config \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
# Only what the build needs, so editing docs or tests doesn't trigger a rebuild.
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
 && cmake --build build -j"$(nproc)"

# ---- runtime ----------------------------------------------------------------
FROM ubuntu:24.04

RUN apt-get update \
 && apt-get install -y --no-install-recommends libsqlite3-0 \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 --no-create-home --shell /usr/sbin/nologin app

WORKDIR /app
COPY --from=build /src/build/bin/http_server ./http_server
COPY static ./static
# The SQLite access log is written under data/; the server runs as "app".
RUN mkdir data && chown app data

USER app
ENV PORT=8080
EXPOSE 8080

# Plain bash, no curl in the image: open a TCP connection, send a request,
# check the status line.
HEALTHCHECK --interval=10s --timeout=3s --start-period=5s --retries=3 \
  CMD ["bash", "-c", "exec 3<>/dev/tcp/127.0.0.1/${PORT} && printf 'GET / HTTP/1.1\\r\\nHost: health\\r\\nConnection: close\\r\\n\\r\\n' >&3 && head -c 12 <&3 | grep -q ' 200'"]

# Exec form, so the server is PID 1 and receives SIGTERM directly for its
# graceful shutdown (a shell wrapper would swallow it).
CMD ["/app/http_server"]
