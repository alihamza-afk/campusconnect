// ---------------------------------------------------------------------------
// db.cpp - the SQLite wrapper's implementation.
//
// The C API needs manual memory management (sqlite3_prepare_v2 / sqlite3_finalize)
// and returns error codes that are easy to ignore by mistake. These classes turn
// that into RAII: constructors acquire, destructors release, and every failed
// call throws a DbError with SQLite's own explanation.
//
// Threading note: cpp-httplib runs handlers on several threads, so the whole
// program shares ONE connection. SQLite's default "serialized" threading mode
// makes a single connection safe to use from many threads (it takes an internal
// lock per API call). Our write transactions are short and single-statement, so
// they never interleave with each other. At larger scale you would open one
// connection per thread or use a connection pool - say that in the viva.
// ---------------------------------------------------------------------------
#include "db.h"

#include <utility>

// ---------------------------------------------------------------------------
// DbError
// ---------------------------------------------------------------------------
DbError::DbError(const std::string& message) : std::runtime_error(message) {}

// ---------------------------------------------------------------------------
// Statement
// ---------------------------------------------------------------------------
Statement::Statement(sqlite3* db, const std::string& sql) : db_(db) {
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
        std::string message = sqlite3_errmsg(db);
        sqlite3_finalize(stmt_);  // safe when null; frees a half-prepared statement
        stmt_ = nullptr;
        throw DbError("prepare failed: " + message);
    }
}

Statement::~Statement() { sqlite3_finalize(stmt_); }

void Statement::check(int rc, const char* what) const {
    if (rc != SQLITE_OK && rc != SQLITE_ROW && rc != SQLITE_DONE) {
        throw DbError(std::string(what) + ": " + sqlite3_errmsg(db_));
    }
}

Statement& Statement::bind(int index, int value) {
    check(sqlite3_bind_int(stmt_, index, value), "bind int");
    return *this;
}

Statement& Statement::bind(int index, long long value) {
    check(sqlite3_bind_int64(stmt_, index, value), "bind int64");
    return *this;
}

Statement& Statement::bind(int index, double value) {
    check(sqlite3_bind_double(stmt_, index, value), "bind double");
    return *this;
}

// Text is copied inside SQLite (SQLITE_TRANSIENT), so the std::string may go out
// of scope safely after the call.
Statement& Statement::bind(int index, const std::string& value) {
    check(sqlite3_bind_text(stmt_, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT),
          "bind text");
    return *this;
}

Statement& Statement::bind(int index, const char* value) {
    check(sqlite3_bind_text(stmt_, index, value, -1, SQLITE_TRANSIENT), "bind text");
    return *this;
}

Statement& Statement::bindNull(int index) {
    check(sqlite3_bind_null(stmt_, index), "bind null");
    return *this;
}

bool Statement::step() {
    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw DbError(std::string("step failed: ") + sqlite3_errmsg(db_));
}

void Statement::run() {
    if (step()) throw DbError("statement returned rows, expected none");
}

bool Statement::columnIsNull(int column) const {
    return sqlite3_column_type(stmt_, column) == SQLITE_NULL;
}

int Statement::columnInt(int column) const { return sqlite3_column_int(stmt_, column); }

long long Statement::columnInt64(int column) const { return sqlite3_column_int64(stmt_, column); }

double Statement::columnDouble(int column) const { return sqlite3_column_double(stmt_, column); }

std::string Statement::columnText(int column) const {
    const unsigned char* text = sqlite3_column_text(stmt_, column);
    if (!text) return "";
    // sqlite3_column_bytes must be read *after* sqlite3_column_text, and
    // contains the byte length, so UTF-8 and embedded NULs survive.
    return std::string(reinterpret_cast<const char*>(text),
                       static_cast<size_t>(sqlite3_column_bytes(stmt_, column)));
}

// ---------------------------------------------------------------------------
// Database
// ---------------------------------------------------------------------------
Database::Database(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        std::string message = db_ ? sqlite3_errmsg(db_) : "out of memory";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw DbError("cannot open database '" + path + "': " + message);
    }
    // Wait up to 5 s for a lock instead of failing instantly with SQLITE_BUSY.
    sqlite3_busy_timeout(db_, 5000);
    // WAL: readers never block the writer (and the writer never blocks readers),
    // and a crash cannot leave a half-written commit behind.
    exec("PRAGMA journal_mode = WAL");
    // NORMAL is the standard WAL setting: fast, still crash-safe.
    exec("PRAGMA synchronous = NORMAL");
    // Enforce the REFERENCES clauses in the schema instead of silently ignoring them.
    exec("PRAGMA foreign_keys = ON");
}

Database::~Database() {
    if (db_) sqlite3_close(db_);
}

void Database::exec(const std::string& sql) {
    char* error = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        std::string message = error ? error : "unknown error";
        sqlite3_free(error);
        throw DbError("SQL failed: " + message + " | in: " + sql.substr(0, 120));
    }
}

Statement Database::prepare(const std::string& sql) { return Statement(db_, sql); }

long long Database::lastInsertRowId() const { return sqlite3_last_insert_rowid(db_); }

int Database::changes() const { return sqlite3_changes(db_); }

int Database::userVersion() {
    Statement s = prepare("PRAGMA user_version");
    return s.step() ? s.columnInt(0) : 0;
}

void Database::setUserVersion(int version) {
    // PRAGMA values cannot be bound, but this is an int we produced ourselves,
    // so there is nothing a user could inject here.
    exec("PRAGMA user_version = " + std::to_string(version));
}

// ---------------------------------------------------------------------------
// Transaction
// ---------------------------------------------------------------------------
Transaction::Transaction(Database& db) : db_(db) { db_.exec("BEGIN IMMEDIATE"); }

Transaction::~Transaction() {
    if (finished_) return;
    try {
        db_.exec("ROLLBACK");
    } catch (...) {
        // A failing rollback must not throw from a destructor.
    }
}

void Transaction::commit() {
    db_.exec("COMMIT");
    finished_ = true;
}
