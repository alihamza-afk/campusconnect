#pragma once
// ---------------------------------------------------------------------------
// db.h - DECLARATIONS for the SQLite wrapper.
//
// The implementation lives in db.cpp. This header only tells the compiler what
// exists (class names, method signatures, member layout). Any .cpp file that
// includes it can use these classes; the linker finds the bodies in db.o.
//
// Why split it like this? A header is included by several .cpp files; if the
// bodies were here too, the compiler would recompile all of them every time
// db.cpp changed, and two definitions of the same function would break the
// One Definition Rule. Declarations in the header + one definition in a .cpp
// is the normal C++ way to keep compile times low and the linker happy.
//
// Classes: Database (owns the connection), Statement (one prepared statement),
// Transaction (BEGIN / COMMIT / ROLLBACK, rolled back by the destructor).
// ---------------------------------------------------------------------------
#include <sqlite3.h>

#include <stdexcept>
#include <string>

// Any failure from SQLite becomes this exception. The message always includes
// sqlite3_errmsg(), which says what went wrong in plain words.
class DbError : public std::runtime_error {
public:
    explicit DbError(const std::string& message);
};

// ---------------------------------------------------------------------------
// Statement - one prepared statement, with typed bind() and column accessors.
// Bind positions are 1-based, exactly like "?" placeholders in SQL.
// ---------------------------------------------------------------------------
class Statement {
    sqlite3_stmt* stmt_ = nullptr;
    sqlite3* db_ = nullptr;

    // Turns a non-OK SQLite return code into a DbError.
    void check(int rc, const char* what) const;

public:
    // Prepares the SQL. Throws on a syntax error, so mistakes show up immediately.
    Statement(sqlite3* db, const std::string& sql);
    ~Statement();

    // A prepared statement owns a C pointer, so copying would double-free it.
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    // Values are bound as data, never concatenated into the SQL text, which is
    // what makes SQL injection impossible.
    Statement& bind(int index, int value);
    Statement& bind(int index, long long value);
    Statement& bind(int index, double value);
    Statement& bind(int index, const std::string& value);
    Statement& bind(int index, const char* value);
    Statement& bindNull(int index);

    // Advances one row. true = a row is available, false = finished.
    bool step();
    // For INSERT / UPDATE / DELETE: runs the statement and expects no rows back.
    void run();

    bool columnIsNull(int column) const;
    int columnInt(int column) const;
    long long columnInt64(int column) const;
    double columnDouble(int column) const;
    std::string columnText(int column) const;
};

// ---------------------------------------------------------------------------
// Database - owns the sqlite3* connection.
// ---------------------------------------------------------------------------
class Database {
    sqlite3* db_ = nullptr;

public:
    // "path" can be a file name (persistent) or ":memory:" (tests).
    explicit Database(const std::string& path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Runs one or more statements. Use for DDL and PRAGMAs. Never for data that
    // comes from a user - that is what prepare() + bind() is for.
    void exec(const std::string& sql);

    Statement prepare(const std::string& sql);

    // Row id assigned by the last INSERT (the INTEGER PRIMARY KEY value).
    long long lastInsertRowId() const;
    // Number of rows changed by the last INSERT / UPDATE / DELETE.
    int changes() const;

    // Schema version kept in the database header; see migrate() in schema.h.
    int userVersion();
    void setUserVersion(int version);
};

// ---------------------------------------------------------------------------
// Transaction - RAII: if a function returns or throws early, the destructor
// runs ROLLBACK, so a half-finished write can never be saved.
// ---------------------------------------------------------------------------
class Transaction {
    Database& db_;
    bool finished_ = false;

public:
    explicit Transaction(Database& db);
    ~Transaction();  // rolls back unless commit() was called

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void commit();
};
