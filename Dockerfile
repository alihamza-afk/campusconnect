# ---------- build stage ----------
FROM gcc:13

# libsodium: Argon2id password hashing. libsqlite3: the database. Both are free.
RUN apt-get update \
 && apt-get install -y --no-install-recommends libsodium-dev libsqlite3-dev libcurl4-openssl-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /app/backend
COPY backend/ ./
COPY frontend/ /app/frontend/

# Pinned versions + checksums: every build uses exactly this library code,
# and the build fails if a download was changed or corrupted.
RUN curl -fsSL -o httplib.h https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.18.3/httplib.h \
 && curl -fsSL -o json.hpp https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp \
 && echo "a0a0c13dc086663863dbe6730a19716f7d3744904b6205496e8301750814dbd5  httplib.h" | sha256sum -c - \
 && echo "9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6  json.hpp" | sha256sum -c - \
 && g++ -std=c++17 -O2 -Wall -Wextra *.cpp -o campusconnect -pthread -lsodium -lsqlite3 -lcurl

# Run as an unprivileged user, not root. The database lives in /app/backend/data.
RUN useradd --system --no-create-home campus \
 && mkdir -p /app/backend/data \
 && chown campus /app/backend/data
USER campus
VOLUME /app/backend/data

EXPOSE 8080
CMD ["./campusconnect"]
