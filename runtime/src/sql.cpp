// marina runtime — std module: sql (SQLite via the system libsqlite3).
//
// Connections live in a runtime table (not ARC-managed): a
// `sql.Connection` object stores an id; `db.close()` removes it.
// MySQL/Postgres/MariaDB drivers are not available yet.
//
// Row classes: one virtual class per distinct column set, created on the
// first `db.query(...)` result. Ids start at 1_000_000 so they never
// clash with compiler-assigned class ids.
#include "runtime_internal.hpp"

#include <sqlite3.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace marina_rt {

namespace {

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

struct SqlConns {
    std::mutex mu;
    std::unordered_map<int64_t, sqlite3*> conns;
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

// Connection id out of a `sql.Connection` object (field 0).
int64_t sql_conn_id(void* o) {
    ObjectObj* obj = reinterpret_cast<ObjectObj*>(o);
    return obj->data[0].i;
}

// Look up a connection's db handle by its registered id (used by the
// async entry points, which capture the id instead of the connection
// object).
sqlite3* sql_conn_ptr_by_id(int64_t id) {
    std::lock_guard<std::mutex> lk(sql_conns().mu);
    auto it = sql_conns().conns.find(id);
    if (it == sql_conns().conns.end()) {
        rt_panic("sql: connection is closed");
    }
    return it->second;
}

// Shared execute body, parameterized by connection id so the async
// variant can run it on a worker thread.
void sql_exec_on(int64_t conn_id, const std::string& sql_s, int64_t n_binds,
                 const MValue* binds, int32_t result_class, MValue* out) {
    sqlite3* db = sql_conn_ptr_by_id(conn_id);
    sqlite3_stmt* stmt = sql_prepare(db, sql_s);
    for (int64_t i = 0; i < n_binds; i++) {
        sql_bind_mv(stmt, (int)(i + 1), binds[i]);
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        sqlite3_finalize(stmt);
        rt_panic("sql: statement returned rows (use db.query() instead): `" + sql_s +
                 "`");
    }
    if (rc != SQLITE_DONE) {
        std::string msg = sqlite_errmsg(db);
        sqlite3_finalize(stmt);
        rt_panic("sql: execute failed: " + msg + " (in `" + sql_s + "`)");
    }
    sqlite3_finalize(stmt);
    void* obj = marina_object_new(result_class, 2);
    reinterpret_cast<ObjectObj*>(obj)->data[0] = mv_int(sqlite3_last_insert_rowid(db));
    reinterpret_cast<ObjectObj*>(obj)->data[1] = mv_int(sqlite3_changes(db));
    out_mv(out, MValue{TAG_OBJECT, 0, 0, 0.0, obj});
}

// Shared query body, parameterized by connection id so the async variant
// can run it on a worker thread.
void sql_query_on(int64_t conn_id, const std::string& sql_s, int64_t n_binds,
                  const MValue* binds, MValue* out) {
    sqlite3* db = sql_conn_ptr_by_id(conn_id);
    sqlite3_stmt* stmt = sql_prepare(db, sql_s);
    for (int64_t i = 0; i < n_binds; i++) {
        sql_bind_mv(stmt, (int)(i + 1), binds[i]);
    }
    void* arr = nullptr;
    int32_t class_id = 0;
    size_t ncols = 0;
    for (;;) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            std::string msg = sqlite_errmsg(db);
            sqlite3_finalize(stmt);
            rt_panic("sql: query failed: " + msg + " (in `" + sql_s + "`)");
        }
        if (arr == nullptr) {
            ncols = (size_t)sqlite3_column_count(stmt);
            // virtual row class: fields = column names
            std::vector<std::string> names;
            names.reserve(ncols);
            for (size_t i = 0; i < ncols; i++) {
                const char* n = reinterpret_cast<const char*>(sqlite3_column_name(
                    stmt, (int)i));
                names.emplace_back(n ? n : "");
            }
            std::string key;
            for (size_t i = 0; i < names.size(); i++) {
                if (i) key += ",";
                key += names[i];
            }
            {
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
            arr = marina_array_new(TAG_OBJECT);
        }
        void* obj = marina_object_new(class_id, (int64_t)ncols);
        ObjectObj* oo = reinterpret_cast<ObjectObj*>(obj);
        for (size_t i = 0; i < ncols; i++) {
            MValue v = MValue{TAG_VOID, 0, 0, 0.0, nullptr};
            switch (sqlite3_column_type(stmt, (int)i)) {
                case SQLITE_INTEGER:
                    v = mv_int(sqlite3_column_int64(stmt, (int)i));
                    break;
                case SQLITE_FLOAT: {
                    v.tag = TAG_FLOAT;
                    v.f = sqlite3_column_double(stmt, (int)i);
                    break;
                }
                case SQLITE_TEXT:
                case SQLITE_BLOB: {
                    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(
                        sqlite3_column_text(stmt, (int)i));
                    int n = sqlite3_column_bytes(stmt, (int)i);
                    v = mv_str_owned(utf8_lossy(bytes, n > 0 ? (size_t)n : 0));
                    break;
                }
                default:
                    break; // NULL -> void/undefined
            }
            oo->data[i] = v;
        }
        MValue obj_mv{TAG_OBJECT, 0, 0, 0.0, obj};
        marina_array_push(arr, &obj_mv);
        drop_mv(obj_mv);
    }
    sqlite3_finalize(stmt);
    if (arr == nullptr) {
        // no rows: still hand back an empty array
        arr = marina_array_new(TAG_OBJECT);
    }
    out_mv(out, MValue{TAG_ARRAY, 0, 0, 0.0, arr});
}


} // namespace

} // namespace marina_rt

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------

extern "C" void marina_sql_open(const MValue* driver, const MValue* url, int32_t class_id,
                     MValue* out) {
    std::string drv = marina_rt::mv_to_string(*driver);
    if (drv != "sqlite") {
        marina_rt::rt_panic(
            "sql: driver '" + drv +
            "' is not supported yet (only 'sqlite' is supported in v0.2; "
            "mysql, pg and mariadb are planned)");
    }
    std::string url_s = marina_rt::mv_to_string(*url);
    if (url_s.find('\0') != std::string::npos) {
        marina_rt::rt_panic("sql: path contains a NUL byte");
    }
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(url_s.c_str(), &db,
                        marina_rt::OPEN_READWRITE | marina_rt::OPEN_CREATE |
                            marina_rt::OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        std::string msg = (db == nullptr) ? url_s : marina_rt::sqlite_errmsg(db);
        marina_rt::rt_panic("sql: cannot open database '" + url_s + "' (" + msg + ")");
    }
    std::lock_guard<std::mutex> lk(marina_rt::sql_conns().mu);
    int64_t id = marina_rt::sql_conns().next_id;
    marina_rt::sql_conns().next_id += 1;
    marina_rt::sql_conns().conns.emplace(id, db);
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
    sqlite3* db;
    {
        std::lock_guard<std::mutex> lk(marina_rt::sql_conns().mu);
        auto it = marina_rt::sql_conns().conns.find(id);
        if (it == marina_rt::sql_conns().conns.end()) {
            marina_rt::rt_panic("sql: connection is closed");
        }
        db = it->second;
        marina_rt::sql_conns().conns.erase(it);
    }
    sqlite3_close_v2(db);
}