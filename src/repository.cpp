module;

#include <folly/json.h>
#include <folly/dynamic.h>
#include <pqxx/pqxx>

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

module ageof.repository;

namespace ageof {
namespace {

using Json = folly::dynamic;

ApiResponse error(int status, std::string message) {
  return {status, folly::toJson(Json::object("error", std::move(message)))};
}

ApiResponse response(int status, Json data) {
  return {status, folly::toJson(data)};
}

std::int64_t parseId(std::string_view text) {
  std::int64_t id{};
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), id);
  if (ec != std::errc{} || end != text.data() + text.size() || id <= 0) {
    throw std::invalid_argument("Invalid id");
  }
  return id;
}

std::string requiredString(const Json& value, const char* key) {
  if (!value.isObject() || !value.contains(key) || !value[key].isString()) {
    throw std::invalid_argument(std::string("Missing string field: ") + key);
  }
  auto result = value[key].asString();
  if (result.empty() || result.size() > 160) {
    throw std::invalid_argument(std::string("Invalid field: ") + key);
  }
  return result;
}

int optionalInt(const Json& value, const char* key, int fallback, int minimum, int maximum) {
  if (!value.contains(key)) {
    return fallback;
  }
  if (!value[key].isInt()) {
    throw std::invalid_argument(std::string("Invalid integer field: ") + key);
  }
  const int result = value[key].asInt();
  if (result < minimum || result > maximum) {
    throw std::invalid_argument(std::string("Out of range field: ") + key);
  }
  return result;
}

Json tournaments(pqxx::transaction_base& tx) {
  Json result = Json::array;
  const auto rows = tx.exec(
      "SELECT t.id, t.slug, t.name, t.game, t.status, t.format, "
      "t.starts_at, t.prize_cents, t.max_players, t.description, t.accent, t.rated, "
      "count(e.player_id)::int AS entrants "
      "FROM tournaments t LEFT JOIN tournament_entries e ON e.tournament_id = t.id "
      "GROUP BY t.id ORDER BY t.starts_at DESC");

  for (const auto& row : rows) {
    result.push_back(Json::object
        ("id", row["id"].as<std::int64_t>())
        ("slug", row["slug"].as<std::string>())
        ("name", row["name"].as<std::string>())
        ("game", row["game"].as<std::string>())
        ("status", row["status"].as<std::string>())
        ("format", row["format"].as<std::string>())
        ("startsAt", row["starts_at"].as<std::string>())
        ("prizeCents", row["prize_cents"].as<int>())
        ("maxPlayers", row["max_players"].is_null() ? Json() : Json(row["max_players"].as<int>()))
        ("entrants", row["entrants"].as<int>())
        ("description", row["description"].as<std::string>())
        ("accent", row["accent"].as<std::string>())
        ("rated", row["rated"].as<bool>()));
  }
  return result;
}

Json matches(pqxx::transaction_base& tx, std::int64_t tournamentId = 0) {
  Json result = Json::array;
  const auto rows = tournamentId == 0
      ? tx.exec(
            "SELECT m.id, m.tournament_id, t.name AS tournament, m.round_name, "
              "m.match_number, m.map_name, m.best_of, m.status, m.scheduled_at, m.stream_url, "
              "COALESCE(json_agg(json_build_object('id', p.id, 'handle', p.handle, "
              "'civilization', p.civilization, 'score', mp.score, 'placement', mp.placement, "
              "'eloBefore', mp.elo_before, 'eloAfter', mp.elo_after) "
              "ORDER BY mp.placement NULLS LAST, mp.score DESC NULLS LAST, p.handle) "
              "FILTER (WHERE p.id IS NOT NULL), '[]'::json)::text AS participants "
            "FROM matches m JOIN tournaments t ON t.id = m.tournament_id "
              "LEFT JOIN match_players mp ON mp.match_id = m.id "
              "LEFT JOIN players p ON p.id = mp.player_id "
              "GROUP BY m.id, t.name ORDER BY m.scheduled_at LIMIT 32")
      : tx.exec(
              "SELECT m.id, m.tournament_id, t.name AS tournament, m.round_name, "
              "m.match_number, m.map_name, m.best_of, m.status, m.scheduled_at, m.stream_url, "
              "COALESCE(json_agg(json_build_object('id', p.id, 'handle', p.handle, "
              "'civilization', p.civilization, 'score', mp.score, 'placement', mp.placement, "
              "'eloBefore', mp.elo_before, 'eloAfter', mp.elo_after) "
              "ORDER BY mp.placement NULLS LAST, mp.score DESC NULLS LAST, p.handle) "
              "FILTER (WHERE p.id IS NOT NULL), '[]'::json)::text AS participants "
            "FROM matches m JOIN tournaments t ON t.id = m.tournament_id "
              "LEFT JOIN match_players mp ON mp.match_id = m.id "
              "LEFT JOIN players p ON p.id = mp.player_id "
              "WHERE m.tournament_id = $1 GROUP BY m.id, t.name ORDER BY m.scheduled_at",
            pqxx::params{tournamentId});

  for (const auto& row : rows) {
    Json item = Json::object
        ("id", row["id"].as<std::int64_t>())
        ("tournamentId", row["tournament_id"].as<std::int64_t>())
        ("tournament", row["tournament"].as<std::string>())
        ("round", row["round_name"].as<std::string>())
        ("matchNumber", row["match_number"].as<int>())
        ("mapName", row["map_name"].as<std::string>())
        ("bestOf", row["best_of"].as<int>())
        ("status", row["status"].as<std::string>())
        ("scheduledAt", row["scheduled_at"].as<std::string>());
    item["streamUrl"] = row["stream_url"].is_null() ? Json() : Json(row["stream_url"].as<std::string>());
      item["participants"] = folly::parseJson(row["participants"].as<std::string>());
    result.push_back(std::move(item));
  }
  return result;
}

Json matchGames(pqxx::transaction_base& tx, std::int64_t matchId) {
  Json result = Json::array;
  for (const auto& row : tx.exec(
           "SELECT g.id, g.game_number, g.map_name, g.played_at, "
           "COALESCE(json_agg(json_build_object('id', p.id, 'handle', p.handle, "
           "'country', p.country, 'civilization', gp.civilization, 'score', gp.score, "
           "'placement', gp.placement) ORDER BY gp.placement, p.handle) "
           "FILTER (WHERE p.id IS NOT NULL), '[]'::json)::text AS participants "
           "FROM match_games g LEFT JOIN match_game_players gp ON gp.game_id = g.id "
           "LEFT JOIN players p ON p.id = gp.player_id "
           "WHERE g.match_id = $1 GROUP BY g.id ORDER BY g.game_number",
           pqxx::params{matchId})) {
    result.push_back(Json::object
        ("id", row["id"].as<std::int64_t>())
        ("gameNumber", row["game_number"].as<int>())
        ("mapName", row["map_name"].as<std::string>())
        ("playedAt", row["played_at"].as<std::string>())
        ("participants", folly::parseJson(row["participants"].as<std::string>())));
  }
  return result;
}

Json players(pqxx::transaction_base& tx) {
  Json result = Json::array;
  for (const auto& row : tx.exec(
           "SELECT p.id, p.handle, p.country, p.civilization, p.elo, "
           "count(e.tournament_id)::int AS events "
           "FROM players p LEFT JOIN tournament_entries e ON e.player_id = p.id "
           "GROUP BY p.id ORDER BY p.elo DESC LIMIT 32")) {
    result.push_back(Json::object
        ("id", row["id"].as<std::int64_t>())
        ("handle", row["handle"].as<std::string>())
        ("country", row["country"].as<std::string>())
        ("civilization", row["civilization"].as<std::string>())
        ("elo", row["elo"].as<int>())
        ("events", row["events"].as<int>()));
  }
  return result;
}

}  // namespace

struct Repository::Impl {
  std::string connectionInfo;

  pqxx::connection& connection() const {
    thread_local std::string threadConninfo;
    thread_local std::unique_ptr<pqxx::connection> threadConnection;
    if (!threadConnection || threadConninfo != connectionInfo || !threadConnection->is_open()) {
      threadConnection = std::make_unique<pqxx::connection>(connectionInfo);
      threadConninfo = connectionInfo;
    }
    return *threadConnection;
  }
};

Repository::Repository() : impl_(std::make_unique<Impl>()) {
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') {
    throw std::runtime_error("HOME is not set");
  }
  const auto passfile = (std::filesystem::path(home) / ".pgpass").string();
  if (!std::filesystem::exists(passfile)) {
    throw std::runtime_error("PostgreSQL passfile is missing");
  }
  impl_->connectionInfo = "host=127.0.0.1 port=5432 dbname=ageof3 user=app connect_timeout=3 passfile='" +
      passfile + "' application_name=ageofserver";
}

Repository::~Repository() = default;

void Repository::ping() const {
  pqxx::read_transaction tx{impl_->connection()};
  (void)tx.exec("SELECT 1").one_row();
  tx.commit();
}

ApiResponse Repository::dispatch(std::string_view method,
                                 std::string_view path,
                                 std::string_view body) const {
  if (method == "GET" && path == "/api/health") {
    pqxx::read_transaction tx{impl_->connection()};
    (void)tx.exec("SELECT 1").one_row();
    tx.commit();
    return response(200, Json::object("ok", true)("database", "ageof3"));
  }

  if (method == "GET" && path == "/api/dashboard") {
    pqxx::read_transaction tx{impl_->connection()};
    const auto totals = tx.exec(
        "SELECT count(*) FILTER (WHERE status <> 'completed')::int AS tournaments, "
        "count(*) FILTER (WHERE status = 'live')::int AS live "
      "FROM tournaments").one_row();
    const auto playerCount = tx.query_value<int>("SELECT count(*)::int FROM players");
    const auto matchCount = tx.exec(
        "SELECT count(*) FILTER (WHERE status = 'scheduled')::int AS scheduled, "
        "count(*) FILTER (WHERE status = 'completed')::int AS completed "
      "FROM matches").one_row();
    Json data = Json::object
        ("counts", Json::object
            ("tournaments", totals["tournaments"].as<int>())
            ("live", totals["live"].as<int>())
            ("players", playerCount)
            ("scheduledMatches", matchCount["scheduled"].as<int>())
            ("completedMatches", matchCount["completed"].as<int>()))
        ("tournaments", tournaments(tx))
        ("matches", matches(tx))
        ("players", players(tx));
    tx.commit();
    return response(200, std::move(data));
  }

  if (method == "GET" && path == "/api/tournaments") {
    pqxx::read_transaction tx{impl_->connection()};
    auto data = tournaments(tx);
    tx.commit();
    return response(200, std::move(data));
  }

  if (method == "GET" && path == "/api/matches") {
    pqxx::read_transaction tx{impl_->connection()};
    auto data = matches(tx);
    tx.commit();
    return response(200, std::move(data));
  }

  constexpr std::string_view matchPrefix = "/api/matches/";
  constexpr std::string_view gamesSuffix = "/games";
  if (method == "GET" && path.starts_with(matchPrefix) && path.ends_with(gamesSuffix)) {
    const auto idText = path.substr(matchPrefix.size(), path.size() - matchPrefix.size() - gamesSuffix.size());
    const auto matchId = parseId(idText);
    pqxx::read_transaction tx{impl_->connection()};
    const auto found = tx.exec("SELECT id FROM matches WHERE id = $1", pqxx::params{matchId});
    if (found.empty()) {
      tx.abort();
      return error(404, "Match not found");
    }
    auto data = matchGames(tx, matchId);
    tx.commit();
    return response(200, std::move(data));
  }

  if (method == "POST" && path.starts_with(matchPrefix) && path.ends_with(gamesSuffix)) {
    const auto idText = path.substr(matchPrefix.size(), path.size() - matchPrefix.size() - gamesSuffix.size());
    const auto matchId = parseId(idText);
    const auto input = folly::parseJson(std::string(body));
    const int gameNumber = optionalInt(input, "gameNumber", 1, 1, 99);
    const auto mapName = requiredString(input, "mapName");
    if (!input.contains("participants") || !input["participants"].isArray() || input["participants"].size() < 2) {
      throw std::invalid_argument("A game needs at least two participants");
    }

    struct GamePlayer {
      std::int64_t id;
      std::string civilization;
      int score;
      int placement{0};
    };
    std::vector<GamePlayer> participants;
    std::unordered_set<std::int64_t> uniqueIds;
    participants.reserve(input["participants"].size());
    for (const auto& value : input["participants"]) {
      if (!value.isObject() || !value.contains("playerId") || !value["playerId"].isInt() ||
          !value.contains("score") || !value["score"].isInt()) {
        throw std::invalid_argument("Each participant needs an integer playerId and score");
      }
      const auto playerId = static_cast<std::int64_t>(value["playerId"].asInt());
      const auto civilization = requiredString(value, "civilization");
      const int score = value["score"].asInt();
      if (playerId <= 0 || score < 0 || score > 99 || !uniqueIds.insert(playerId).second) {
        throw std::invalid_argument("Participant ids must be unique and scores must be between 0 and 99");
      }
      participants.push_back({playerId, civilization, score, 0});
    }
    std::sort(participants.begin(), participants.end(), [](const auto& left, const auto& right) {
      return left.score > right.score;
    });
    for (std::size_t index = 0; index < participants.size(); ++index) {
      if (index == 0 || participants[index].score != participants[index - 1].score) {
        participants[index].placement = static_cast<int>(index + 1);
      } else {
        participants[index].placement = participants[index - 1].placement;
      }
    }

    pqxx::work tx{impl_->connection()};
    const auto match = tx.exec("SELECT status, best_of FROM matches WHERE id = $1 FOR UPDATE", pqxx::params{matchId});
    if (match.empty()) {
      tx.abort();
      return error(404, "Match not found");
    }
    if (match[0]["status"].as<std::string>() == "completed") {
      tx.abort();
      return error(409, "Match is already completed");
    }
    const int bestOf = match[0]["best_of"].as<int>();
    if (gameNumber > bestOf) {
      tx.abort();
      return error(409, "Game number exceeds the series best-of limit");
    }
    const int existingGames = tx.exec(
        "SELECT count(*)::int FROM match_games WHERE match_id = $1", pqxx::params{matchId})
        .one_row()[0].as<int>();
    if (gameNumber != existingGames + 1) {
      tx.abort();
      return error(409, "Games must be recorded in sequence");
    }
    const int winsRequired = bestOf / 2 + 1;
    const int seriesWinners = tx.exec(
        "SELECT count(*)::int FROM match_players WHERE match_id = $1 AND score >= $2",
        pqxx::params{matchId, winsRequired}).one_row()[0].as<int>();
    if (seriesWinners > 0) {
      tx.abort();
      return error(409, "The best-of series has already been decided");
    }
    const auto rosterSize = tx.exec(
        "SELECT count(*)::int FROM match_players WHERE match_id = $1", pqxx::params{matchId})
        .one_row()[0].as<int>();
    if (rosterSize != static_cast<int>(participants.size())) {
      tx.abort();
      return error(400, "Each game must include every match participant");
    }
    for (const auto& participant : participants) {
      const bool belongsToMatch = tx.exec(
          "SELECT EXISTS (SELECT 1 FROM match_players WHERE match_id = $1 AND player_id = $2)",
          pqxx::params{matchId, participant.id}).one_row()[0].as<bool>();
      if (!belongsToMatch) {
        tx.abort();
        return error(400, "Every game participant must belong to the match");
      }
    }
    const auto inserted = tx.exec(
        "INSERT INTO match_games (match_id, game_number, map_name) VALUES ($1, $2, $3) "
        "ON CONFLICT (match_id, game_number) DO NOTHING RETURNING id",
        pqxx::params{matchId, gameNumber, mapName});
    if (inserted.empty()) {
      tx.abort();
      return error(409, "That game number is already recorded");
    }
    const auto gameId = inserted[0]["id"].as<std::int64_t>();
    for (const auto& participant : participants) {
      tx.exec("INSERT INTO match_game_players (game_id, player_id, civilization, score, placement) "
              "VALUES ($1, $2, $3, $4, $5)",
              pqxx::params{gameId, participant.id, participant.civilization,
                           participant.score, participant.placement});
    }
    tx.exec("UPDATE match_players mp SET score = ("
        "SELECT count(*)::smallint FROM match_games g "
        "JOIN match_game_players gp ON gp.game_id = g.id "
        "WHERE g.match_id = mp.match_id AND gp.player_id = mp.player_id AND gp.placement = 1) "
        "WHERE mp.match_id = $1",
        pqxx::params{matchId});
    tx.exec("UPDATE matches SET status = 'live' WHERE id = $1 AND status = 'scheduled'",
            pqxx::params{matchId});
    Json seriesScores = Json::array;
    for (const auto& row : tx.exec(
         "SELECT player_id, score::int FROM match_players WHERE match_id = $1 ORDER BY player_id",
         pqxx::params{matchId})) {
      seriesScores.push_back(Json::object
        ("playerId", row["player_id"].as<std::int64_t>())
        ("score", row["score"].as<int>()));
    }
    tx.commit();
    return response(201, Json::object("id", gameId)("gameNumber", gameNumber)
      ("mapName", mapName)("participants", static_cast<int>(participants.size()))
      ("seriesScores", std::move(seriesScores)));
  }

  if (method == "GET" && path == "/api/players") {
    pqxx::read_transaction tx{impl_->connection()};
    auto data = players(tx);
    tx.commit();
    return response(200, std::move(data));
  }

  if (method == "GET" && path == "/api/analytics") {
    pqxx::read_transaction tx{impl_->connection()};
    Json civilizations = Json::array;
    for (const auto& row : tx.exec(
             "SELECT gp.civilization, count(*)::int AS games, "
             "count(*) FILTER (WHERE gp.placement = 1)::int AS wins, "
             "round(100.0 * count(*) FILTER (WHERE gp.placement = 1) / count(*), 1)::float8 AS win_rate "
             "FROM match_game_players gp JOIN match_games g ON g.id = gp.game_id "
             "JOIN matches m ON m.id = g.match_id WHERE m.status = 'completed' "
             "GROUP BY gp.civilization ORDER BY win_rate DESC, games DESC, gp.civilization")) {
      civilizations.push_back(Json::object
          ("civilization", row["civilization"].as<std::string>())
          ("games", row["games"].as<int>())
          ("wins", row["wins"].as<int>())
          ("winRate", row["win_rate"].as<double>()));
    }

    Json maps = Json::array;
    for (const auto& row : tx.exec(
             "SELECT g.map_name, gp.civilization, count(*)::int AS games, "
             "count(*) FILTER (WHERE gp.placement = 1)::int AS wins, "
             "round(100.0 * count(*) FILTER (WHERE gp.placement = 1) / count(*), 1)::float8 AS win_rate "
             "FROM match_game_players gp JOIN match_games g ON g.id = gp.game_id "
             "JOIN matches m ON m.id = g.match_id WHERE m.status = 'completed' "
             "GROUP BY g.map_name, gp.civilization ORDER BY g.map_name, win_rate DESC, games DESC")) {
      maps.push_back(Json::object
          ("mapName", row["map_name"].as<std::string>())
          ("civilization", row["civilization"].as<std::string>())
          ("games", row["games"].as<int>())
          ("wins", row["wins"].as<int>())
          ("winRate", row["win_rate"].as<double>()));
    }

    Json matchups = Json::array;
    for (const auto& row : tx.exec(
             "SELECT a.civilization, b.civilization AS opponent_civilization, count(*)::int AS games, "
             "count(*) FILTER (WHERE a.placement < b.placement)::int AS wins, "
             "count(*) FILTER (WHERE a.placement > b.placement)::int AS losses, "
             "count(*) FILTER (WHERE a.placement = b.placement)::int AS draws "
             "FROM match_game_players a JOIN match_game_players b "
             "ON b.game_id = a.game_id AND b.player_id <> a.player_id "
             "JOIN match_games g ON g.id = a.game_id JOIN matches m ON m.id = g.match_id "
             "WHERE m.status = 'completed' AND a.civilization <> b.civilization "
             "GROUP BY a.civilization, b.civilization ORDER BY games DESC, a.civilization, b.civilization")) {
      matchups.push_back(Json::object
          ("civilization", row["civilization"].as<std::string>())
          ("opponentCivilization", row["opponent_civilization"].as<std::string>())
          ("games", row["games"].as<int>())
          ("wins", row["wins"].as<int>())
          ("losses", row["losses"].as<int>())
          ("draws", row["draws"].as<int>()));
    }
    tx.commit();
    return response(200, Json::object("civilizations", std::move(civilizations))
        ("maps", std::move(maps))("matchups", std::move(matchups)));
  }

  if (method == "POST" && path == "/api/tournaments") {
    const auto input = folly::parseJson(std::string(body));
    const auto name = requiredString(input, "name");
    const auto format = requiredString(input, "format");
    const auto description = input.contains("description") && input["description"].isString()
        ? input["description"].asString() : std::string{};
    const auto startsAt = requiredString(input, "startsAt");
    const int maxPlayers = optionalInt(input, "maxPlayers", 0, 0, 10000);
    const int prizeCents = optionalInt(input, "prizeCents", 0, 0, 100000000);
    if (input.contains("rated") && !input["rated"].isBool()) {
      throw std::invalid_argument("rated must be a boolean");
    }
    const bool rated = !input.contains("rated") || input["rated"].asBool();
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto slug = "community-" + std::to_string(unique);

    pqxx::work tx{impl_->connection()};
    const auto row = tx.exec(
        "INSERT INTO tournaments (slug, name, status, format, starts_at, prize_cents, max_players, description, rated) "
        "VALUES ($1, $2, 'registration', $3, $4::timestamptz, $5, NULLIF($6, 0), $7, $8) RETURNING id",
      pqxx::params{slug, name, format, startsAt, prizeCents, maxPlayers, description, rated}).one_row();
    const auto id = row["id"].as<std::int64_t>();
    tx.commit();
    return response(201, Json::object("id", id)("slug", slug)("name", name));
  }

  constexpr std::string_view registerPrefix = "/api/tournaments/";
  constexpr std::string_view registerSuffix = "/entries";
  if (method == "POST" && path.starts_with(registerPrefix) && path.ends_with(registerSuffix)) {
    const auto idText = path.substr(registerPrefix.size(), path.size() - registerPrefix.size() - registerSuffix.size());
    const auto tournamentId = parseId(idText);
    const auto input = folly::parseJson(std::string(body));
    const auto handle = requiredString(input, "handle");
    const auto civilization = requiredString(input, "civilization");
    const auto country = input.contains("country") && input["country"].isString()
        ? input["country"].asString() : std::string("XX");
    if (country.size() != 2) {
      throw std::invalid_argument("Country must be a two-letter code");
    }

    pqxx::work tx{impl_->connection()};
    const auto tournament = tx.exec(
        "SELECT status, max_players, (SELECT count(*) FROM tournament_entries WHERE tournament_id = t.id) AS entrants "
        "FROM tournaments t WHERE id = $1",
      pqxx::params{tournamentId});
    if (tournament.empty()) {
      tx.abort();
      return error(404, "Tournament not found");
    }
    const auto state = tournament[0]["status"].as<std::string>();
    if (state != "registration" && state != "upcoming") {
      tx.abort();
      return error(409, "Registration is closed");
    }
    const auto player = tx.exec(
        "INSERT INTO players (handle, country, civilization) VALUES ($1, $2, $3) "
        "ON CONFLICT (handle) DO UPDATE SET country = EXCLUDED.country, civilization = EXCLUDED.civilization "
        "RETURNING id",
      pqxx::params{handle, country, civilization}).one_row();
      const auto playerId = player["id"].as<std::int64_t>();
        const auto alreadyEntered = tx.exec(
        "SELECT EXISTS (SELECT 1 FROM tournament_entries WHERE tournament_id = $1 AND player_id = $2)",
          pqxx::params{tournamentId, playerId}).one_row()[0].as<bool>();
      if (!alreadyEntered && !tournament[0]["max_players"].is_null() &&
        tournament[0]["entrants"].as<std::int64_t>() >= tournament[0]["max_players"].as<int>()) {
        tx.abort();
        return error(409, "Tournament is full");
      }
    const auto row = tx.exec(
        "INSERT INTO tournament_entries (tournament_id, player_id, seed) "
        "VALUES ($1, $2, (SELECT count(*) + 1 FROM tournament_entries WHERE tournament_id = $1)) "
        "ON CONFLICT (tournament_id, player_id) DO UPDATE SET status = 'confirmed' "
        "RETURNING tournament_id, player_id",
      pqxx::params{tournamentId, playerId}).one_row();
    tx.commit();
    return response(201, Json::object("tournamentId", row["tournament_id"].as<std::int64_t>())
      ("playerId", row["player_id"].as<std::int64_t>())("handle", handle));
  }

  constexpr std::string_view createMatchSuffix = "/matches";
  if (method == "POST" && path.starts_with(registerPrefix) && path.ends_with(createMatchSuffix)) {
    const auto idText = path.substr(registerPrefix.size(), path.size() - registerPrefix.size() - createMatchSuffix.size());
    const auto tournamentId = parseId(idText);
    const auto input = folly::parseJson(std::string(body));
    const auto round = requiredString(input, "round");
    const auto mapName = requiredString(input, "mapName");
    const auto scheduledAt = requiredString(input, "scheduledAt");
    const int matchNumber = optionalInt(input, "matchNumber", 1, 1, 100000);
    const int bestOf = optionalInt(input, "bestOf", 1, 1, 99);
    if (!input.contains("playerIds") || !input["playerIds"].isArray() || input["playerIds"].size() < 2) {
      throw std::invalid_argument("A match needs at least two registered players");
    }
    std::vector<std::int64_t> playerIds;
    std::unordered_set<std::int64_t> uniqueIds;
    playerIds.reserve(input["playerIds"].size());
    for (const auto& value : input["playerIds"]) {
      if (!value.isInt()) {
        throw std::invalid_argument("Player ids must be integers");
      }
      const auto playerId = static_cast<std::int64_t>(value.asInt());
      if (playerId <= 0 || !uniqueIds.insert(playerId).second) {
        throw std::invalid_argument("Player ids must be unique positive integers");
      }
      playerIds.push_back(playerId);
    }

    pqxx::work tx{impl_->connection()};
    const auto inserted = tx.exec(
        "INSERT INTO matches (tournament_id, round_name, match_number, map_name, best_of, status, scheduled_at) "
        "VALUES ($1, $2, $3, $4, $5, 'scheduled', $6::timestamptz) "
        "ON CONFLICT (tournament_id, round_name, match_number) DO NOTHING RETURNING id",
      pqxx::params{tournamentId, round, matchNumber, mapName, bestOf, scheduledAt});
    if (inserted.empty()) {
      tx.abort();
      return error(409, "That match number is already used in this round");
    }
    const auto matchId = inserted[0]["id"].as<std::int64_t>();
    for (const auto playerId : playerIds) {
        const auto added = tx.exec(
          "INSERT INTO match_players (match_id, player_id) "
          "SELECT $1, e.player_id FROM tournament_entries e "
          "WHERE e.tournament_id = $2 AND e.player_id = $3 RETURNING player_id",
          pqxx::params{matchId, tournamentId, playerId});
      if (added.empty()) {
        throw std::invalid_argument("Every match player must be registered in the tournament");
      }
    }
    tx.commit();
    return response(201, Json::object("id", matchId)("mapName", mapName)("participants", static_cast<int>(playerIds.size())));
  }

  constexpr std::string_view resultPrefix = "/api/matches/";
  constexpr std::string_view resultSuffix = "/result";
  if (method == "POST" && path.starts_with(resultPrefix) && path.ends_with(resultSuffix)) {
    const auto idText = path.substr(resultPrefix.size(), path.size() - resultPrefix.size() - resultSuffix.size());
    const auto matchId = parseId(idText);
    const auto input = folly::parseJson(std::string(body));
    const auto mapName = requiredString(input, "mapName");
    if (!input.contains("participants") || !input["participants"].isArray() || input["participants"].size() < 2) {
      throw std::invalid_argument("A result needs scores for at least two participants");
    }
    struct ScoredPlayer {
      std::int64_t id;
      int score;
      int placement;
      int eloBefore{0};
      int eloAfter{0};
    };
    std::vector<ScoredPlayer> participants;
    std::unordered_set<std::int64_t> uniqueIds;
    participants.reserve(input["participants"].size());
    for (const auto& value : input["participants"]) {
      if (!value.isObject() || !value.contains("playerId") || !value["playerId"].isInt() ||
          !value.contains("score") || !value["score"].isInt()) {
        throw std::invalid_argument("Each result needs an integer playerId and score");
      }
      const auto playerId = static_cast<std::int64_t>(value["playerId"].asInt());
      const int score = value["score"].asInt();
      if (playerId <= 0 || score < 0 || score > 99 || !uniqueIds.insert(playerId).second) {
        throw std::invalid_argument("Participant ids must be unique and scores must be between 0 and 99");
      }
      participants.push_back({playerId, score, 0, 0, 0});
    }
    std::sort(participants.begin(), participants.end(), [](const auto& left, const auto& right) {
      return left.score > right.score;
    });
    for (std::size_t i = 0; i < participants.size(); ++i) {
      if (i == 0 || participants[i].score != participants[i - 1].score) {
        participants[i].placement = static_cast<int>(i + 1);
      } else {
        participants[i].placement = participants[i - 1].placement;
      }
    }

    pqxx::work tx{impl_->connection()};
    const auto matchInfo = tx.exec(
        "SELECT best_of FROM matches "
        "WHERE id = $1 AND status IN ('scheduled', 'live') AND elo_applied = FALSE FOR UPDATE",
        pqxx::params{matchId});
    if (matchInfo.empty()) {
      tx.abort();
      return error(404, "Match not found or already completed");
    }
    const int bestOf = matchInfo[0]["best_of"].as<int>();
    std::unordered_set<std::int64_t> matchRoster;
    for (const auto& rosterEntry : tx.exec(
             "SELECT player_id FROM match_players WHERE match_id = $1 FOR UPDATE",
             pqxx::params{matchId})) {
      matchRoster.insert(rosterEntry["player_id"].as<std::int64_t>());
    }
    if (matchRoster.size() != participants.size() || std::any_of(participants.begin(), participants.end(),
          [&](const auto& participant) { return !matchRoster.contains(participant.id); })) {
      tx.abort();
      return error(400, "Result participants must exactly match the scheduled match roster");
    }

    const int gamesPlayed = tx.exec(
        "SELECT count(*)::int FROM match_games WHERE match_id = $1", pqxx::params{matchId})
        .one_row()[0].as<int>();
    if (gamesPlayed > 0) {
      const int winsRequired = bestOf / 2 + 1;
      const int seriesWinners = tx.exec(
          "SELECT count(*)::int FROM match_players WHERE match_id = $1 AND score >= $2",
          pqxx::params{matchId, winsRequired}).one_row()[0].as<int>();
      if (seriesWinners == 0) {
        tx.abort();
        return error(409, "The best-of series has not been decided yet");
      }
      for (const auto& participant : participants) {
        const auto recordedScore = tx.exec(
            "SELECT score FROM match_players WHERE match_id = $1 AND player_id = $2",
            pqxx::params{matchId, participant.id}).one_row()[0].as<int>();
        if (recordedScore != participant.score) {
          tx.abort();
          return error(409, "Series scores must match the recorded game results");
        }
      }
    }

    const auto row = tx.exec(
      "UPDATE matches SET map_name = $2, score_a = NULL, score_b = NULL, status = 'completed', elo_applied = TRUE "
      "WHERE id = $1 AND status IN ('scheduled', 'live') AND elo_applied = FALSE "
      "RETURNING tournament_id",
        pqxx::params{matchId, mapName});
    if (row.empty()) {
      tx.abort();
      return error(404, "Match not found or already completed");
    }
    const auto tournamentId = row[0]["tournament_id"].as<std::int64_t>();
    tx.exec("DELETE FROM match_players WHERE match_id = $1", pqxx::params{matchId});
    for (const auto& participant : participants) {
        const auto added = tx.exec(
          "INSERT INTO match_players (match_id, player_id, score, placement) "
          "SELECT $1, e.player_id, $4, $5 FROM tournament_entries e "
          "WHERE e.tournament_id = $2 AND e.player_id = $3 RETURNING player_id",
          pqxx::params{matchId, tournamentId, participant.id, participant.score, participant.placement});
      if (added.empty()) {
        throw std::invalid_argument("Every result participant must be registered in the tournament");
      }
    }

    const bool rated = tx.exec(
      "SELECT rated FROM tournaments WHERE id = $1", pqxx::params{tournamentId}).one_row()[0].as<bool>();
    std::vector<std::int64_t> lockOrder;
    lockOrder.reserve(participants.size());
    for (const auto& participant : participants) {
      lockOrder.push_back(participant.id);
    }
    std::sort(lockOrder.begin(), lockOrder.end());
    std::unordered_map<std::int64_t, int> oldRatings;
    oldRatings.reserve(lockOrder.size());
    for (const auto playerId : lockOrder) {
        const auto player = tx.exec(
          "SELECT elo FROM players WHERE id = $1 FOR UPDATE", pqxx::params{playerId}).one_row();
      oldRatings.emplace(playerId, player["elo"].as<int>());
    }

    for (auto& participant : participants) {
      participant.eloBefore = oldRatings.at(participant.id);
      double delta = 0.0;
      if (rated) {
        for (const auto& opponent : participants) {
          if (opponent.id == participant.id) continue;
          const double expected = 1.0 / (1.0 + std::pow(10.0,
              (oldRatings.at(opponent.id) - participant.eloBefore) / 400.0));
          const double actual = participant.placement < opponent.placement ? 1.0
              : participant.placement > opponent.placement ? 0.0 : 0.5;
          delta += 32.0 * (actual - expected) / static_cast<double>(participants.size() - 1);
        }
      }
      participant.eloAfter = std::max(0, static_cast<int>(std::lround(participant.eloBefore + delta)));
    }

    for (const auto& participant : participants) {
      if (rated) {
        tx.exec("UPDATE players SET elo = $2 WHERE id = $1",
          pqxx::params{participant.id, participant.eloAfter});
      }
        tx.exec(
          "UPDATE match_players SET elo_before = $3, elo_after = $4 "
          "WHERE match_id = $1 AND player_id = $2",
          pqxx::params{matchId, participant.id, participant.eloBefore,
                 rated ? participant.eloAfter : participant.eloBefore});
    }

    tx.exec(
        "UPDATE tournaments SET status = 'completed' WHERE id = $1 "
        "AND EXISTS (SELECT 1 FROM matches WHERE tournament_id = $1) "
        "AND NOT EXISTS (SELECT 1 FROM matches WHERE tournament_id = $1 AND status <> 'completed')",
      pqxx::params{tournamentId});
    tx.commit();
    Json updatedParticipants = Json::array;
    for (const auto& participant : participants) {
      updatedParticipants.push_back(Json::object
          ("playerId", participant.id)
          ("placement", participant.placement)
          ("score", participant.score)
          ("eloBefore", participant.eloBefore)
          ("eloAfter", rated ? participant.eloAfter : participant.eloBefore));
    }
    return response(200, Json::object("id", matchId)("mapName", mapName)
      ("rated", rated)("participants", std::move(updatedParticipants))("status", "completed"));
  }

  if (method == "GET" && path.starts_with("/api/tournaments/")) {
    const auto id = parseId(path.substr(std::string_view("/api/tournaments/").size()));
    pqxx::read_transaction tx{impl_->connection()};
    const auto rows = tx.exec(
        "SELECT id, slug, name, game, status, format, starts_at, prize_cents, max_players, description, accent, rated "
        "FROM tournaments WHERE id = $1",
      pqxx::params{id});
    if (rows.empty()) {
      tx.abort();
      return error(404, "Tournament not found");
    }
    const auto& row = rows[0];
    Json data = Json::object
        ("id", row["id"].as<std::int64_t>())
        ("slug", row["slug"].as<std::string>())
        ("name", row["name"].as<std::string>())
        ("game", row["game"].as<std::string>())
        ("status", row["status"].as<std::string>())
        ("format", row["format"].as<std::string>())
        ("startsAt", row["starts_at"].as<std::string>())
        ("prizeCents", row["prize_cents"].as<int>())
        ("maxPlayers", row["max_players"].is_null() ? Json() : Json(row["max_players"].as<int>()))
        ("rated", row["rated"].as<bool>())
        ("description", row["description"].as<std::string>())
        ("accent", row["accent"].as<std::string>());
    data["matches"] = matches(tx, id);
    Json entries = Json::array;
    for (const auto& entry : tx.exec(
             "SELECT p.id, p.handle, p.country, p.civilization, p.elo, e.seed, e.status "
             "FROM tournament_entries e JOIN players p ON p.id = e.player_id "
             "WHERE e.tournament_id = $1 ORDER BY e.seed, p.handle",
         pqxx::params{id})) {
      entries.push_back(Json::object
          ("id", entry["id"].as<std::int64_t>())
          ("handle", entry["handle"].as<std::string>())
          ("country", entry["country"].as<std::string>())
          ("civilization", entry["civilization"].as<std::string>())
          ("elo", entry["elo"].as<int>())
          ("seed", entry["seed"].as<int>())
          ("status", entry["status"].as<std::string>()));
    }
    data["entries"] = std::move(entries);
    tx.commit();
    return response(200, std::move(data));
  }

  return error(404, "Route not found");
}

}  // namespace ageof
