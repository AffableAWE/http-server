-- Schema for the http-server access log database.
-- Run once at server startup; CREATE TABLE IF NOT EXISTS makes it idempotent.

CREATE TABLE IF NOT EXISTS access_logs (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp   TEXT    NOT NULL DEFAULT (datetime('now')),
    client_ip   TEXT    NOT NULL,
    method      TEXT    NOT NULL,
    path        TEXT    NOT NULL,
    status_code INTEGER NOT NULL,
    bytes_sent  INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_access_logs_timestamp
    ON access_logs(timestamp);

CREATE INDEX IF NOT EXISTS idx_access_logs_status
    ON access_logs(status_code);
