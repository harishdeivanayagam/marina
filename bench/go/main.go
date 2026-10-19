// Go port of examples/todos-http-sqlite.tsm
// Run: go run . (module deps download automatically)
package main

import (
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"strconv"

	_ "modernc.org/sqlite"
	"database/sql"
)

const (
	dbPath = "/tmp/todos-http-sqlite.db"
	port   = 8080
)

type Todo struct {
	ID   int64  `json:"id"`
	Text string `json:"text"`
	Done int64  `json:"done"`
}

var db *sql.DB

type TodoInput struct {
	Text string `json:"text"`
}

func listTodos(w http.ResponseWriter, r *http.Request) {
	rows, err := db.Query("SELECT id, text, done FROM todos ORDER BY id")
	if err != nil {
		http.Error(w, err.Error(), 500)
		return
	}
	defer rows.Close()
	todos := []Todo{}
	for rows.Next() {
		var t Todo
		if err := rows.Scan(&t.ID, &t.Text, &t.Done); err != nil {
			http.Error(w, err.Error(), 500)
			return
		}
		todos = append(todos, t)
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(todos)
}

func createTodo(w http.ResponseWriter, r *http.Request) {
	var in TodoInput
	if err := json.NewDecoder(r.Body).Decode(&in); err != nil || len(in.Text) < 4 || len(in.Text) > 20 {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(map[string]string{"error": "invalid todo"})
		return
	}
	res, err := db.Exec("INSERT INTO todos (text, done) VALUES (?, 0)", in.Text)
	if err != nil {
		http.Error(w, err.Error(), 500)
		return
	}
	id, _ := res.LastInsertId()
	rows, err := db.Query("SELECT id, text, done FROM todos WHERE id = ?", id)
	if err != nil {
		http.Error(w, err.Error(), 500)
		return
	}
	defer rows.Close()
	todos := []Todo{}
	for rows.Next() {
		var t Todo
		if err := rows.Scan(&t.ID, &t.Text, &t.Done); err != nil {
			http.Error(w, err.Error(), 500)
			return
		}
		todos = append(todos, t)
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(todos)
}

func doneAll(w http.ResponseWriter, r *http.Request) {
	if _, err := db.Exec("UPDATE todos SET done = 1"); err != nil {
		http.Error(w, err.Error(), 500)
		return
	}
	listTodos(w, r)
}

func main() {
	var err error
	db, err = sql.Open("sqlite", dbPath)
	if err != nil {
		log.Fatal(err)
	}
	if _, err := db.Exec("CREATE TABLE IF NOT EXISTS todos (id INTEGER PRIMARY KEY AUTOINCREMENT, text TEXT NOT NULL, done INTEGER NOT NULL DEFAULT 0)"); err != nil {
		log.Fatal(err)
	}

	mux := http.NewServeMux()
	mux.HandleFunc("GET /todos", listTodos)
	mux.HandleFunc("GET /todos/", listTodos)
	mux.HandleFunc("POST /todos", createTodo)
	mux.HandleFunc("POST /todos/", createTodo)
	mux.HandleFunc("POST /todos/done-all", doneAll)

	log.Printf("Server is running on http://localhost:%s/todos", strconv.Itoa(port))
	log.Fatal(http.ListenAndServe(fmt.Sprintf("0.0.0.0:%d", port), mux))
}
