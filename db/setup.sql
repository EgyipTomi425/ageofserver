CREATE TABLE IF NOT EXISTS tournaments (
    id BIGSERIAL PRIMARY KEY,
    slug TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL,
    game TEXT NOT NULL DEFAULT 'Age of Empires III: Definitive Edition',
    status TEXT NOT NULL CHECK (status IN ('registration', 'upcoming', 'live', 'completed')),
    format TEXT NOT NULL,
    starts_at TIMESTAMPTZ NOT NULL,
    prize_cents INTEGER NOT NULL DEFAULT 0 CHECK (prize_cents >= 0),
    max_players INTEGER CHECK (max_players IS NULL OR max_players > 1),
    rated BOOLEAN NOT NULL DEFAULT TRUE,
    description TEXT NOT NULL DEFAULT '',
    accent TEXT NOT NULL DEFAULT '#d45b32',
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS players (
    id BIGSERIAL PRIMARY KEY,
    handle TEXT NOT NULL UNIQUE,
    country CHAR(2) NOT NULL,
    civilization TEXT NOT NULL,
    elo INTEGER NOT NULL DEFAULT 1200,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS tournament_entries (
    tournament_id BIGINT NOT NULL REFERENCES tournaments(id) ON DELETE CASCADE,
    player_id BIGINT NOT NULL REFERENCES players(id) ON DELETE CASCADE,
    seed INTEGER NOT NULL DEFAULT 0,
    status TEXT NOT NULL DEFAULT 'confirmed' CHECK (status IN ('pending', 'confirmed', 'eliminated', 'champion')),
    registered_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (tournament_id, player_id)
);

CREATE TABLE IF NOT EXISTS matches (
    id BIGSERIAL PRIMARY KEY,
    tournament_id BIGINT NOT NULL REFERENCES tournaments(id) ON DELETE CASCADE,
    round_name TEXT NOT NULL,
    match_number INTEGER NOT NULL,
    player_a_id BIGINT REFERENCES players(id) ON DELETE SET NULL,
    player_b_id BIGINT REFERENCES players(id) ON DELETE SET NULL,
    map_name TEXT NOT NULL DEFAULT 'Unknown Map',
    elo_applied BOOLEAN NOT NULL DEFAULT FALSE,
    score_a SMALLINT,
    score_b SMALLINT,
    best_of SMALLINT NOT NULL DEFAULT 3 CHECK (best_of > 0),
    status TEXT NOT NULL DEFAULT 'scheduled' CHECK (status IN ('scheduled', 'live', 'completed')),
    scheduled_at TIMESTAMPTZ NOT NULL,
    stream_url TEXT,
    UNIQUE (tournament_id, round_name, match_number),
    CHECK ((score_a IS NULL AND score_b IS NULL) OR (score_a >= 0 AND score_b >= 0))
);

ALTER TABLE tournaments ALTER COLUMN max_players DROP NOT NULL;
ALTER TABLE tournaments ADD COLUMN IF NOT EXISTS rated BOOLEAN NOT NULL DEFAULT TRUE;
ALTER TABLE matches ADD COLUMN IF NOT EXISTS map_name TEXT NOT NULL DEFAULT 'Unknown Map';
ALTER TABLE matches ADD COLUMN IF NOT EXISTS elo_applied BOOLEAN NOT NULL DEFAULT FALSE;

CREATE TABLE IF NOT EXISTS match_players (
    match_id BIGINT NOT NULL REFERENCES matches(id) ON DELETE CASCADE,
    player_id BIGINT NOT NULL REFERENCES players(id) ON DELETE CASCADE,
    score SMALLINT CHECK (score IS NULL OR score >= 0),
    placement SMALLINT CHECK (placement IS NULL OR placement > 0),
    elo_before INTEGER,
    elo_after INTEGER,
    PRIMARY KEY (match_id, player_id)
);

ALTER TABLE match_players ADD COLUMN IF NOT EXISTS elo_before INTEGER;
ALTER TABLE match_players ADD COLUMN IF NOT EXISTS elo_after INTEGER;

CREATE TABLE IF NOT EXISTS match_games (
    id BIGSERIAL PRIMARY KEY,
    match_id BIGINT NOT NULL REFERENCES matches(id) ON DELETE CASCADE,
    game_number SMALLINT NOT NULL CHECK (game_number > 0),
    map_name TEXT NOT NULL,
    played_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (match_id, game_number)
);

CREATE TABLE IF NOT EXISTS match_game_players (
    game_id BIGINT NOT NULL REFERENCES match_games(id) ON DELETE CASCADE,
    player_id BIGINT NOT NULL REFERENCES players(id) ON DELETE CASCADE,
    civilization TEXT NOT NULL,
    score SMALLINT NOT NULL CHECK (score BETWEEN 0 AND 99),
    placement SMALLINT NOT NULL CHECK (placement > 0),
    PRIMARY KEY (game_id, player_id)
);

CREATE INDEX IF NOT EXISTS matches_schedule_idx ON matches (scheduled_at, status);
CREATE INDEX IF NOT EXISTS entries_tournament_idx ON tournament_entries (tournament_id, seed);
CREATE INDEX IF NOT EXISTS match_games_match_idx ON match_games (match_id, game_number);
CREATE INDEX IF NOT EXISTS match_game_players_civ_idx ON match_game_players (civilization, game_id);

INSERT INTO tournaments (slug, name, status, format, starts_at, prize_cents, max_players, description, accent)
VALUES
    ('fall-invitational-2026', 'Fall Invitational', 'live', 'Double elimination · Bo3', now() - interval '45 minutes', 250000, 16, 'Meghívásos szezonközi torna, nyolc kiemelt játékossal.', '#d45b32'),
    ('frontier-open-2026', 'Frontier Open', 'registration', 'Single elimination · Bo3', now() + interval '3 days', 120000, 32, 'Nyílt kupa új és visszatérő játékosoknak.', '#287b73'),
    ('autumn-league-finals-2026', 'Autumn League Finals', 'completed', 'Round robin · Bo5', now() - interval '8 days', 400000, 8, 'Az őszi liga legjobb négy játékosának döntője.', '#4768a8')
ON CONFLICT (slug) DO NOTHING;

INSERT INTO players (handle, country, civilization, elo)
VALUES
    ('MapleFox', 'CA', 'Haudenosaunee', 1842),
    ('BlueComet', 'DE', 'Swedes', 1791),
    ('Rook', 'US', 'British', 1764),
    ('Sable', 'FR', 'French', 1728),
    ('Northstar', 'SE', 'Swedes', 1699),
    ('Coyote', 'MX', 'Aztecs', 1661),
    ('Juniper', 'GB', 'British', 1617),
    ('Tidecaller', 'NL', 'Dutch', 1588),
    ('Copperfield', 'PL', 'Polish', 1542),
    ('Lynx', 'FI', 'Inca', 1511),
    ('Mistral', 'ES', 'Spanish', 1484),
    ('Orchid', 'BR', 'Japanese', 1455),
    ('EmberVale', 'AU', 'Mongols', 1438),
    ('NightHarbor', 'KR', 'Koreans', 1412),
    ('Stoneglass', 'NO', 'Russians', 1389),
    ('Frostline', 'RU', 'Ottomans', 1376),
    ('SignalFox', 'JP', 'Japanese', 1348),
    ('Quartz', 'IT', 'Italians', 1327),
    ('Morrow', 'CZ', 'Spanish', 1305),
    ('Riverbound', 'CN', 'Chinese', 1289)
ON CONFLICT (handle) DO NOTHING;

INSERT INTO tournament_entries (tournament_id, player_id, seed, status)
SELECT t.id, p.id, v.seed, v.status
FROM (VALUES
    ('fall-invitational-2026', 'MapleFox', 1, 'confirmed'),
    ('fall-invitational-2026', 'BlueComet', 2, 'confirmed'),
    ('fall-invitational-2026', 'Rook', 3, 'confirmed'),
    ('fall-invitational-2026', 'Sable', 4, 'confirmed'),
    ('fall-invitational-2026', 'Northstar', 5, 'confirmed'),
    ('fall-invitational-2026', 'Coyote', 6, 'confirmed'),
    ('fall-invitational-2026', 'Juniper', 7, 'confirmed'),
    ('fall-invitational-2026', 'Tidecaller', 8, 'confirmed'),
    ('frontier-open-2026', 'Copperfield', 1, 'confirmed'),
    ('frontier-open-2026', 'Lynx', 2, 'confirmed'),
    ('frontier-open-2026', 'Mistral', 3, 'confirmed'),
    ('frontier-open-2026', 'Orchid', 4, 'confirmed'),
    ('frontier-open-2026', 'EmberVale', 5, 'confirmed'),
    ('frontier-open-2026', 'NightHarbor', 6, 'confirmed'),
    ('frontier-open-2026', 'Stoneglass', 7, 'confirmed'),
    ('frontier-open-2026', 'Frostline', 8, 'confirmed'),
    ('frontier-open-2026', 'SignalFox', 9, 'confirmed'),
    ('frontier-open-2026', 'Quartz', 10, 'confirmed'),
    ('frontier-open-2026', 'Morrow', 11, 'confirmed'),
    ('frontier-open-2026', 'Riverbound', 12, 'confirmed'),
    ('autumn-league-finals-2026', 'MapleFox', 1, 'champion'),
    ('autumn-league-finals-2026', 'BlueComet', 2, 'confirmed'),
    ('autumn-league-finals-2026', 'Rook', 3, 'confirmed'),
    ('autumn-league-finals-2026', 'Sable', 4, 'confirmed')
) AS v(tournament_slug, player_handle, seed, status)
JOIN tournaments t ON t.slug = v.tournament_slug
JOIN players p ON p.handle = v.player_handle
ON CONFLICT (tournament_id, player_id) DO NOTHING;

INSERT INTO matches (tournament_id, round_name, match_number, player_a_id, player_b_id, map_name, score_a, score_b, best_of, status, scheduled_at, stream_url)
SELECT t.id, v.round_name, v.match_number, pa.id, pb.id, v.map_name, v.score_a, v.score_b, v.best_of, v.status, v.scheduled_at, v.stream_url
FROM (VALUES
    ('fall-invitational-2026', 'Upper bracket · Round 1', 1, 'MapleFox', 'Tidecaller', 'Great Plains', 2, 0, 3, 'completed', now() - interval '35 minutes', NULL),
    ('fall-invitational-2026', 'Upper bracket · Round 1', 2, 'BlueComet', 'Juniper', 'Deccan', 1, 1, 3, 'live', now() - interval '8 minutes', 'https://twitch.tv/eso-community'),
    ('fall-invitational-2026', 'Upper bracket · Round 1', 3, 'Rook', 'Coyote', 'Yucatan', NULL, NULL, 3, 'scheduled', now() + interval '20 minutes', 'https://twitch.tv/eso-community'),
    ('fall-invitational-2026', 'Upper bracket · Round 1', 4, 'Sable', 'Northstar', 'Bayou', NULL, NULL, 3, 'scheduled', now() + interval '55 minutes', NULL),
    ('autumn-league-finals-2026', 'Grand final', 1, 'MapleFox', 'BlueComet', 'Great Plains', 3, 1, 5, 'completed', now() - interval '7 days', NULL),
    ('autumn-league-finals-2026', 'Semi-final', 2, 'Rook', 'Sable', 'High Plains', 3, 2, 5, 'completed', now() - interval '8 days', NULL)
) AS v(tournament_slug, round_name, match_number, player_a, player_b, map_name, score_a, score_b, best_of, status, scheduled_at, stream_url)
JOIN tournaments t ON t.slug = v.tournament_slug
JOIN players pa ON pa.handle = v.player_a
JOIN players pb ON pb.handle = v.player_b
ON CONFLICT (tournament_id, round_name, match_number) DO NOTHING;

UPDATE matches m
SET map_name = CASE
        WHEN t.slug = 'fall-invitational-2026' AND m.match_number = 1 THEN 'Great Plains'
        WHEN t.slug = 'fall-invitational-2026' AND m.match_number = 2 THEN 'Deccan'
        WHEN t.slug = 'fall-invitational-2026' AND m.match_number = 3 THEN 'Yucatan'
        WHEN t.slug = 'fall-invitational-2026' AND m.match_number = 4 THEN 'Bayou'
        WHEN t.slug = 'autumn-league-finals-2026' AND m.round_name = 'Grand final' THEN 'Great Plains'
        WHEN t.slug = 'autumn-league-finals-2026' THEN 'High Plains'
        ELSE m.map_name
END
FROM tournaments t
WHERE m.tournament_id = t.id
    AND m.map_name = 'Unknown Map';

UPDATE matches SET elo_applied = TRUE WHERE status = 'completed' AND elo_applied = FALSE;

INSERT INTO match_players (match_id, player_id, score, placement)
SELECT m.id, m.player_a_id, m.score_a, CASE WHEN m.status = 'completed' AND m.score_a > m.score_b THEN 1 ELSE NULL END
FROM matches m
WHERE m.player_a_id IS NOT NULL
ON CONFLICT (match_id, player_id) DO NOTHING;

INSERT INTO match_players (match_id, player_id, score, placement)
SELECT m.id, m.player_b_id, m.score_b, CASE WHEN m.status = 'completed' AND m.score_b > m.score_a THEN 1 ELSE NULL END
FROM matches m
WHERE m.player_b_id IS NOT NULL
ON CONFLICT (match_id, player_id) DO NOTHING;

-- Synthetic demonstration game logs; these are not imported tournament results.
INSERT INTO match_games (match_id, game_number, map_name, played_at)
SELECT m.id, v.game_number, v.map_name, m.scheduled_at + v.game_number * interval '12 minutes'
FROM (VALUES
    ('autumn-league-finals-2026', 'Grand final', 1, 1, 'Great Plains'),
    ('autumn-league-finals-2026', 'Grand final', 1, 2, 'Deccan'),
    ('autumn-league-finals-2026', 'Grand final', 1, 3, 'Bayou'),
    ('autumn-league-finals-2026', 'Grand final', 1, 4, 'Yucatan')
) AS v(tournament_slug, round_name, match_number, game_number, map_name)
JOIN tournaments t ON t.slug = v.tournament_slug
JOIN matches m ON m.tournament_id = t.id AND m.round_name = v.round_name AND m.match_number = v.match_number
ON CONFLICT (match_id, game_number) DO NOTHING;

INSERT INTO match_game_players (game_id, player_id, civilization, score, placement)
SELECT g.id, p.id, v.civilization, v.score, v.placement
FROM (VALUES
    ('autumn-league-finals-2026', 'Grand final', 1, 1, 'MapleFox', 'Haudenosaunee', 2, 1),
    ('autumn-league-finals-2026', 'Grand final', 1, 1, 'BlueComet', 'Swedes', 0, 2),
    ('autumn-league-finals-2026', 'Grand final', 1, 2, 'MapleFox', 'British', 1, 2),
    ('autumn-league-finals-2026', 'Grand final', 1, 2, 'BlueComet', 'French', 2, 1),
    ('autumn-league-finals-2026', 'Grand final', 1, 3, 'MapleFox', 'Japanese', 3, 1),
    ('autumn-league-finals-2026', 'Grand final', 1, 3, 'BlueComet', 'Dutch', 1, 2),
    ('autumn-league-finals-2026', 'Grand final', 1, 4, 'MapleFox', 'Spanish', 3, 1),
    ('autumn-league-finals-2026', 'Grand final', 1, 4, 'BlueComet', 'French', 2, 2)
) AS v(tournament_slug, round_name, match_number, game_number, handle, civilization, score, placement)
JOIN tournaments t ON t.slug = v.tournament_slug
JOIN matches m ON m.tournament_id = t.id AND m.round_name = v.round_name AND m.match_number = v.match_number
JOIN match_games g ON g.match_id = m.id AND g.game_number = v.game_number
JOIN players p ON p.handle = v.handle
ON CONFLICT (game_id, player_id) DO NOTHING;
