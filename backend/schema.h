#pragma once
// ---------------------------------------------------------------------------
// schema.h - DECLARATION of the database layout step.
//
// The tables themselves (the SQL text) are an implementation detail and live in
// schema.cpp. Callers only need to know: call migrate(db) once after opening.
// ---------------------------------------------------------------------------
#include "db.h"

// Bump this whenever the schema changes, and add a matching "if (version < N)"
// block in migrate() in schema.cpp.
inline constexpr int SCHEMA_VERSION = 7;

// Creates or upgrades the tables to SCHEMA_VERSION. Safe to call on every start:
// with a current database it does nothing.
void migrate(Database& db);
