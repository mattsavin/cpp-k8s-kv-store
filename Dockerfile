FROM ubuntu:latest AS builder

RUN apt-get update && apt-get install -y build-essential cmake

WORKDIR /src

COPY . .

RUN mkdir build && cd build && cmake .. && cmake --build .

FROM ubuntu:latest

WORKDIR /app

RUN mkdir -p /data

COPY --from=builder /src/build/kv_store /app/kv_store

EXPOSE 8081

CMD ["/app/kv_store"]