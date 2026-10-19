// Bun port of examples/todos-http-sqlite.tsm
// Run: bun main.ts
import { Database } from "bun:sqlite";

const DB_PATH = "/tmp/todos-http-sqlite.db";
const PORT = 8080;

const db = new Database(DB_PATH);
db.run(
  "CREATE TABLE IF NOT EXISTS todos (id INTEGER PRIMARY KEY AUTOINCREMENT, text TEXT NOT NULL, done INTEGER NOT NULL DEFAULT 0)"
);
const stmtAll = db.query("SELECT id, text, done FROM todos ORDER BY id");
const stmtInsert = db.query("INSERT INTO todos (text, done) VALUES (?, 0)");
const stmtById = db.query("SELECT id, text, done FROM todos WHERE id = ?");
const stmtDoneAll = db.query("UPDATE todos SET done = 1");

function validate(body: unknown): { text: string } | null {
  const parsed = body as { text?: unknown };
  if (typeof parsed?.text !== "string" || parsed.text.length < 4 || parsed.text.length > 20) {
    return null;
  }
  return parsed as { text: string };
}

const server = Bun.serve({
  port: PORT,
  hostname: "0.0.0.0",
  async fetch(req) {
    const url = new URL(req.url);
    if (url.pathname === "/todos" || url.pathname === "/todos/") {
      if (req.method === "GET") {
        return Response.json(stmtAll.all());
      }
      if (req.method === "POST") {
        const todo = validate(await req.json());
        if (!todo) {
          return Response.json({ error: "invalid todo" }, { status: 400 });
        }
        const result = stmtInsert.run(todo.text);
        return Response.json(stmtById.all(result.lastInsertRowid));
      }
    }
    if (url.pathname === "/todos/done-all" && req.method === "POST") {
      stmtDoneAll.run();
      return Response.json(stmtAll.all());
    }
    return Response.json({ error: "not found" }, { status: 404 });
  },
});

console.log(`Server is running on http://localhost:${server.port}/todos`);
