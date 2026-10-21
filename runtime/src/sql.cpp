// marina runtime — std module: sql.
//
// Driver dispatch + shared plumbing. A `sql.Connection` object stores
// an id; `db.close()` removes it from the runtime table (connections
// are not ARC-managed). The wire-level work happens in the driver
// backends:
//
//   sqlite -> system libsqlite3 (this file)
//   pg     -> native wire protocol (sql_pg.cpp)
//   mysql  -> native wire protocol (sql_mysql.cpp; "mariadb" shares it)
//
// Row classes: one virtual class per distinct column set, created on
// the first `db.query(...)` result. Ids start at 1_000_000 so they
// never clash with compiler-assigned class ids.
#include "runtime_internal.hpp"
#include "sql_backend.hpp"

#include <sqlite3.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace marina_rt {

namespace {

// Backend kinds stored in the connection table.
constexpr int CONN_SQLITE = 0;
constexpr int CONN_PG = 1;
constexpr int CONN_MYSQL = 2;

// SQLITE_OPEN_FULLMUTEX: serialize all access to the connection
// internally. Required because async db calls may run the same
// connection on several worker threads at once (system SQLite is often
// built THREADSAFE=2, which forbids concurrent same-connection use and
// fails with "disk I/O error").
constexpr int OPEN_READWRITE = 0x2;
constexpr int OPEN_CREATE = 0x4;
constexpr int OPEN_FULLMUTEX = 0x10000;

// SQLITE_TRANSIENT: sqlite copies the bound string.
inline sqlite3_destructor_type transient() {
    return reinterpret_cast<sqlite3_destructor_type>(-1);
}

struct DbConn {
    int kind;
    void* p;
};

struct SqlConns {
    std::mutex mu;
    std::unordered_map<int64_t, DbConn> conns;
    int64_t next_id = 1;
};
SqlConns& sql_conns() {
    static SqlConns* s = new SqlConns(); // leaked
    return *s;
}

struct SqlRowClasses {
    std::mutex mu;
    std::unordered_map<std::string, int32_t> classes;
    int32_t next_id = 1000000;
};
SqlRowClasses& sql_row_classes() {
    static SqlRowClasses* s = new SqlRowClasses(); // leaked
    return *s;
}

std::string sqlite_errmsg(sqlite3* db) { return std::string(sqlite3_errmsg(db)); }

// Open a database and remember the statement, panicking on failure.
sqlite3_stmt* sql_prepare(sqlite3* db, const std::string& sql) {
    if (sql.find('\0') != std::string::npos) {
        rt_panic("sql: statement contains a NUL byte");
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        rt_panic("sql: prepare failed: " + sqlite_errmsg(db) + " (in `" + sql + "`)");
    }
    return stmt;
}

// Bind an MValue to sqlite parameter `idx` (1-based).
void sql_bind_mv(sqlite3_stmt* stmt, int idx, const MValue& v) {
    int rc;
    switch (v.tag) {
        case TAG_INT:
            rc = sqlite3_bind_int64(stmt, idx, v.i);
            break;
        case TAG_FLOAT:
            rc = sqlite3_bind_double(stmt, idx, v.f);
            break;
        case TAG_BOOL:
            rc = sqlite3_bind_int64(stmt, idx, v.i != 0 ? 1 : 0);
            break;
        case TAG_STRING: {
            void* obj = v.p;
            size_t len = (size_t)str_len(obj);
            const uint8_t* bytes = str_bytes(obj);
            rc = sqlite3_bind_text(stmt, idx, reinterpret_cast<const char*>(bytes),
                                   (int)len, transient());
            break;
        }
        case TAG_VOID:
            rc = sqlite3_bind_null(stmt, idx);
            break;
        default:
            rt_panic("sql: cannot bind a value of tag " + std::to_string(v.tag) +
                     " (bind an integer, float, string, bool or null)");
    }
    if (rc != SQLITE_OK) {
        rt_panic("sql: bind failed");
    }
}

} // namespace

// ---------------------------------------------------------------------------
// sqlite backend (SqlRowSet-based interface, shared with the other drivers)
// ---------------------------------------------------------------------------

void* sqlite_connect(const std::string& path) {
    if (path.find('\0') != std::string::npos) {
        rt_panic("sql: path contains a NUL byte");
    }
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db,
                        OPEN_READWRITE | OPEN_CREATE | OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        std::string msg = (db == nullptr) ? path : sqlite_errmsg(db);
        rt_panic("sql: cannot open database '" + path + "' (" + msg + ")");
    }
    return db;
}

void sqlite_disconnect(void* p) {
    sqlite3* db = reinterpret_cast<sqlite3*>(p);
    sqlite3_close_v2(db);
}

void sqlite_execute(void* p, const std::string& sql, const std::vector<MValue>& binds,
                    int64_t* last_id, int64_t* changes) {
    sqlite3* db = reinterpret_cast<sqlite3*>(p);
    sqlite3_stmt* stmt = sql_prepare(db, sql);
    for (size_t i = 0; i < binds.size(); i++) {
        sql_bind_mv(stmt, (int)(i + 1), binds[i]);
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        sqlite3_finalize(stmt);
        rt_panic("sql: statement returned rows (use db.query() instead): `" + sql + "`");
    }
    if (rc != SQLITE_DONE) {
        std::string msg = sqlite_errmsg(db);
        sqlite3_finalize(stmt);
        rt_panic("sql: execute failed: " + msg + " (in `" + sql + "`)");
    }
    sqlite3_finalize(stmt);
    *last_id = sqlite3_last_insert_rowid(db);
    *changes = sqlite3_changes(db);
}

void sqlite_query(void* p, const std::string& sql, const std::vector<MValue>& binds,
                  SqlRowSet* out) {
    sqlite3* db = reinterpret_cast<sqlite3*>(p);
    sqlite3_stmt* stmt = sql_prepare(db, sql);
    for (size_t i = 0; i < binds.size(); i++) {
        sql_bind_mv(stmt, (int)(i + 1), binds[i]);
    }
    for (;;) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            std::string msg = sqlite_errmsg(db);
            sqlite3_finalize(stmt);
            rt_panic("sql: query failed: " + msg + " (in `" + sql + "`)");
        }
        if (out->col_names.empty()) {
            int ncols = sqlite3_column_count(stmt);
            for (int i = 0; i < ncols; i++) {
                const char* n =
                    reinterpret_cast<const char*>(sqlite3_column_name(stmt, i));
                out->col_names.emplace_back(n ? n : "");
            }
        }
        size_t ncols = out->col_names.size();
        std::vector<MValue> row(ncols);
        for (size_t i = 0; i < ncols; i++) {
            switch (sqlite3_column_type(stmt, (int)i)) {
                case SQLITE_INTEGER:
                    row[i] = mv_int(sqlite3_column_int64(stmt, (int)i));
                    break;
                case SQLITE_FLOAT:
                    row[i].tag = TAG_FLOAT;
                    row[i].f = sqlite3_column_double(stmt, (int)i);
                    break;
                case SQLITE_TEXT:
                case SQLITE_BLOB: {
                    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(
                        sqlite3_column_text(stmt, (int)i));
                    int n = sqlite3_column_bytes(stmt, (int)i);
                    row[i] = mv_str_owned(utf8_lossy(bytes, n > 0 ? (size_t)n : 0));
                    break;
                }
                default:
                    break; // NULL -> void/undefined
            }
        }
        out->rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

namespace {

DbConn sql_conn_by_id(int64_t id) {
    std::lock_guard<std::mutex> lk(sql_conns().mu);
    auto it = sql_conns().conns.find(id);
    if (it == sql_conns().conns.end()) {
        rt_panic("sql: connection is closed");
    }
    return it->second;
}

// Connection id out of a `sql.Connection` object (field 0).
int64_t sql_conn_id(void* o) {
    ObjectObj* obj = reinterpret_cast<ObjectObj*>(o);
    return obj->data[0].i;
}

// Shared execute body, parameterized by connection id so the async
// variant can run it on a worker thread.
void sql_exec_on(int64_t conn_id, const std::string& sql_s, int64_t n_binds,
                 const MValue* binds, int32_t result_class, MValue* out) {
    DbConn conn = sql_conn_by_id(conn_id);
    std::vector<MValue> bv(binds, binds + (n_binds > 0 ? (size_t)n_binds : 0));
    int64_t last_id = 0;
    int64_t changes = 0;
    switch (conn.kind) {
        case CONN_SQLITE:
            sqlite_execute(conn.p, sql_s, bv, &last_id, &changes);
            break;
        case CONN_PG:
            pg_execute(conn.p, sql_s, bv, &last_id, &changes);
            break;
        case CONN_MYSQL:
            my_execute(conn.p, sql_s, bv, &last_id, &changes);
            break;
        default:
            rt_panic("sql: unknown driver");
    }
    void* obj = marina_object_new(result_class, 2);
    reinterpret_cast<ObjectObj*>(obj)->data[0] = mv_int(last_id);
    reinterpret_cast<ObjectObj*>(obj)->data[1] = mv_int(changes);
    out_mv(out, MValue{TAG_OBJECT, 0, 0, 0.0, obj});
}

// Shared query body: run the statement on the right backend, then
// materialize the rows into a virtual `sql_Row` class.
void sql_query_on(int64_t conn_id, const std::string& sql_s, int64_t n_binds,
                  const MValue* binds, MValue* out) {
    DbConn conn = sql_conn_by_id(conn_id);
    std::vector<MValue> bv(binds, binds + (n_binds > 0 ? (size_t)n_binds : 0));
    SqlRowSet rs;
    switch (conn.kind) {
        case CONN_SQLITE:
            sqlite_query(conn.p, sql_s, bv, &rs);
            break;
        case CONN_PG:
            pg_query(conn.p, sql_s, bv, &rs);
            break;
        case CONN_MYSQL:
            my_query(conn.p, sql_s, bv, &rs);
            break;
        default:
            rt_panic("sql: unknown driver");
    }
    // virtual row class: fields = column names
    int32_t class_id = 0;
    const std::vector<std::string>& names = rs.col_names;
    std::string key;
    for (size_t i = 0; i < names.size(); i++) {
        if (i) key += ",";
        key += names[i];
    }
    if (!names.empty()) {
        std::lock_guard<std::mutex> lk(sql_row_classes().mu);
        auto it = sql_row_classes().classes.find(key);
        if (it != sql_row_classes().classes.end()) {
            class_id = it->second;
        } else {
            int32_t id = sql_row_classes().next_id;
            sql_row_classes().next_id += 1;
            sql_row_classes().classes.emplace(key, id);
            class_id = id;
            std::vector<uint8_t> meta;
            const char* base = "sql_Row";
            meta.insert(meta.end(), base, base + 7);
            for (const std::string& n : names) {
                meta.push_back(0);
                meta.insert(meta.end(), n.begin(), n.end());
            }
            marina_class_register(id, meta.data(), (int64_t)meta.size());
        }
    }
    void* arr = marina_array_new(TAG_OBJECT);
    for (std::vector<MValue>& row : rs.rows) {
        void* obj = marina_object_new(class_id, (int64_t)row.size());
        ObjectObj* oo = reinterpret_cast<ObjectObj*>(obj);
        for (size_t i = 0; i < row.size(); i++) {
            oo->data[i] = row[i]; // string payloads move into the object
        }
        MValue obj_mv{TAG_OBJECT, 0, 0, 0.0, obj};
        marina_array_push(arr, &obj_mv);
        drop_mv(obj_mv);
    }
    out_mv(out, MValue{TAG_ARRAY, 0, 0, 0.0, arr});
}

} // namespace

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------

extern "C" void marina_sql_open(const MValue* driver, const MValue* url, int32_t class_id,
                     MValue* out) {
    std::string drv = marina_rt::mv_to_string(*driver);
    std::string url_s = marina_rt::mv_to_string(*url);
    int kind;
    if (drv == "sqlite") {
        kind = marina_rt::CONN_SQLITE;
        if (url_s.find('\0') != std::string::npos) {
            marina_rt::rt_panic("sql: path contains a NUL byte");
        }
    } else if (drv == "pg" || drv == "postgres" || drv == "postgresql") {
        kind = marina_rt::CONN_PG;
    } else if (drv == "mysql" || drv == "mariadb") {
        kind = marina_rt::CONN_MYSQL;
    } else {
        marina_rt::rt_panic("sql: unknown driver '" + drv +
                            "' (supported: sqlite, pg, mysql, mariadb)");
    }
    void* p = nullptr;
    switch (kind) {
        case marina_rt::CONN_SQLITE:
            p = marina_rt::sqlite_connect(url_s);
            break;
        case marina_rt::CONN_PG:
            p = marina_rt::pg_connect(url_s);
            break;
        case marina_rt::CONN_MYSQL:
            p = marina_rt::my_connect(url_s);
            break;
    }
    std::lock_guard<std::mutex> lk(marina_rt::sql_conns().mu);
    int64_t id = marina_rt::sql_conns().next_id;
    marina_rt::sql_conns().next_id += 1;
    marina_rt::sql_conns().conns.emplace(id, marina_rt::DbConn{kind, p});
    void* obj = marina_object_new(class_id, 1);
    reinterpret_cast<marina_rt::ObjectObj*>(obj)->data[0] = marina_rt::mv_int(id);
    marina_rt::out_mv(out, MValue{TAG_OBJECT, 0, 0, 0.0, obj});
}

extern "C" void marina_sql_execute(void* o, const MValue* sql, int64_t n_binds,
                        const MValue* binds, int32_t result_class, MValue* out) {
    std::string sql_s = marina_rt::mv_to_string(*sql);
    int64_t conn_id = marina_rt::sql_conn_id(o);
    marina_rt::sql_exec_on(conn_id, sql_s, n_binds, binds, result_class, out);
}

extern "C" void marina_sql_query(void* o, const MValue* sql, int64_t n_binds,
                      const MValue* binds, MValue* out) {
    std::string sql_s = marina_rt::mv_to_string(*sql);
    int64_t conn_id = marina_rt::sql_conn_id(o);
    marina_rt::sql_query_on(conn_id, sql_s, n_binds, binds, out);
}

extern "C" void* marina_sql_execute_async(void* o, const MValue* sql, size_t n_binds,
                               const MValue* binds, int32_t result_class) {
    marina_rt::Owns owns;
    MValue sv = *sql;
    marina_rt::own_into(owns, sv);
    std::vector<MValue> bv;
    bv.reserve(n_binds);
    for (size_t i = 0; i < n_binds; i++) {
        const MValue* b = &binds[i];
        marina_rt::own_into(owns, *b);
        bv.push_back(MValue{b->tag, b->pad, b->i, b->f, b->p});
    }
    marina_rt::retain_ptr(o);
    int64_t conn_id = marina_rt::sql_conn_id(o);
    return marina_rt::spawn_promise_io(
        [conn_id, sv, bv, result_class] {
            std::string sql_s = marina_rt::mv_to_string(sv);
            MValue r = MValue{TAG_VOID, 0, 0, 0.0, nullptr};
            marina_rt::sql_exec_on(conn_id, sql_s, (int64_t)bv.size(), bv.data(),
                                   result_class, &r);
            return r;
        },
        std::move(owns));
}

extern "C" void* marina_sql_query_async(void* o, const MValue* sql, size_t n_binds,
                             const MValue* binds) {
    marina_rt::Owns owns;
    MValue sv = *sql;
    marina_rt::own_into(owns, sv);
    std::vector<MValue> bv;
    bv.reserve(n_binds);
    for (size_t i = 0; i < n_binds; i++) {
        const MValue* b = &binds[i];
        marina_rt::own_into(owns, *b);
        bv.push_back(MValue{b->tag, b->pad, b->i, b->f, b->p});
    }
    marina_rt::retain_ptr(o);
    int64_t conn_id = marina_rt::sql_conn_id(o);
    return marina_rt::spawn_promise_io(
        [conn_id, sv, bv] {
            std::string sql_s = marina_rt::mv_to_string(sv);
            MValue r = MValue{TAG_VOID, 0, 0, 0.0, nullptr};
            marina_rt::sql_query_on(conn_id, sql_s, (int64_t)bv.size(), bv.data(), &r);
            return r;
        },
        std::move(owns));
}

extern "C" void marina_sql_close(void* o) {
    int64_t id = marina_rt::sql_conn_id(o);
    marina_rt::DbConn conn;
    {
        std::lock_guard<std::mutex> lk(marina_rt::sql_conns().mu);
        auto it = marina_rt::sql_conns().conns.find(id);
        if (it == marina_rt::sql_conns().conns.end()) {
            marina_rt::rt_panic("sql: connection is closed");
        }
        conn = it->second;
        marina_rt::sql_conns().conns.erase(it);
    }
    switch (conn.kind) {
        case marina_rt::CONN_SQLITE:
            marina_rt::sqlite_disconnect(conn.p);
            break;
        case marina_rt::CONN_PG:
            marina_rt::pg_disconnect(conn.p);
            break;
        case marina_rt::CONN_MYSQL:
            marina_rt::my_disconnect(conn.p);
            break;
    }
}

} // namespace marina_rt
