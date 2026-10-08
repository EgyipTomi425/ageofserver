# Age of Empires Tournament API

HTTP/2 JSON API backed by PostgreSQL, implemented with Proxygen and C++20 named modules.

## Build and run

Requires CMake 3.28+, Ninja, GCC 14+, Proxygen/Folly and libpqxx 7.

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/ageofserver
```

The API binds to `0.0.0.0:18432` using cleartext HTTP/2 (h2c). Put it behind the configured reverse proxy; do not expose the development listener directly.

## Database

The service connects to the local `ageof3` database as `app` and reads its password from `~/.pgpass`.

```sh
PGPASSFILE="$HOME/.pgpass" psql -h 127.0.0.1 -U app -d ageof3 \
  --single-transaction -v ON_ERROR_STOP=1 -f db/setup.sql
```

The setup script is idempotent and includes clearly fictional sample tournaments and game logs. Analytics are illustrative only, not official ranked data.

## Routes

- `GET /api/health`, `/api/dashboard`, `/api/tournaments`, `/api/matches`, `/api/players`
- `GET /api/matches/{id}/games` loads one series' game history on demand
- `POST /api/matches/{id}/games` records map, player civilizations, and game score
- `GET /api/analytics` derives civilization, map, and matchup tables from completed sample games
- Tournament, entry, and series-result write routes are implemented in `src/repository.cpp`

The server uses Proxygen's event-based network IO and a bounded four-worker database pool. Game-level Elo changes are not applied per game; series Elo is applied once when the series result is submitted.