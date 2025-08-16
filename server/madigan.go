package main

import (
//    "bufio"
    "fmt"
    "log"
    "net"
    "net/http"
//    "strconv"
    "strings"
    "sync"


	"encoding/binary"
	"io"
//	"time"

)

type UIConnection struct {
    Conn     net.Conn
    Messages []string // store all recent messages
    Id       string
    Plugin   string
}

var (
    connections = make(map[string]*UIConnection)
    mu          sync.Mutex
)


const MaxMessageLen = 16 * 1024 * 1024 // 16 MB safety limit

// Read one framed message (4-byte big-endian length + payload).
// Returns the payload slice (owned by caller) or an error.
func ReadMessage(conn net.Conn) ([]byte, error) {
	header := make([]byte, 4)
	if _, err := io.ReadFull(conn, header); err != nil {
		return nil, err
	}
	length := binary.BigEndian.Uint32(header)
	if length == 0 {
		return []byte{}, nil
	}
	if length > MaxMessageLen {
		return nil, fmt.Errorf("message too large: %d", length)
	}
	buf := make([]byte, length)
	if _, err := io.ReadFull(conn, buf); err != nil {
		return nil, err
	}
	return buf, nil
}

// Send a framed message. Ensures full write.
func SendMessage(conn net.Conn, payload []byte) error {
	var header [4]byte
	binary.BigEndian.PutUint32(header[:], uint32(len(payload)))
	// Write header
	if _, err := conn.Write(header[:]); err != nil {
		return err
	}
	// Write payload in loop until all sent
	total := 0
	for total < len(payload) {
		n, err := conn.Write(payload[total:])
		if err != nil {
			return err
		}
		total += n
	}
	return nil
}




func tcpHandler() {
    ln, err := net.Listen("tcp", ":5555")
    if err != nil {
        log.Fatal(err)
    }
    log.Println("TCP server listening on :5555")

    for {
        conn, err := ln.Accept()
        if err != nil {
            continue
        }
        go handleTCPConnection(conn)
    }
}

func decodeMessage(message string) map[string]string {
    parts := strings.Split(message, "||");
    result := map[string]string{}
    for _, part := range parts {
      part_parts := strings.Split(part,"|")
      //log.Printf("%s = %s",part_parts[0], part_parts[1])
      result[part_parts[0]] = part_parts[1]
    }
    return result
}

func encodeMessage(message map[string]string) string {
    result := ""
    for key, value := range message {
       if len(result) > 0 { result = result + "||" }
        result = result + key + "|" + value
    }
    return result
}

func handleTCPConnection(c net.Conn) {
    defer c.Close()
    msg, err := ReadMessage(c)
    if err != nil {
        log.Println("Invalid connection: failure reading id message")
        return
    }
    message := decodeMessage(string(msg))
    id := message["source"]
    plugin:= message["plugin"]
    mu.Lock()
    connections[id] = &UIConnection{Conn: c, Id: id, Plugin: plugin}
    mu.Unlock()

    log.Println("UI connected:", id)

    for {
        ReadMessage(c)
/*
        line, err := reader.ReadString('\n')
        if err != nil {
            log.Println("UI disconnected:", id)
            mu.Lock()
            delete(connections, id)
            mu.Unlock()
            return
        }
        line = strings.TrimSpace(line)

        mu.Lock()
        connections[id].Messages = append(connections[id].Messages, line)
        if len(connections[id].Messages) > 50 { // keep last 50
            connections[id].Messages = connections[id].Messages[len(connections[id].Messages)-50:]
        }
        mu.Unlock()

        log.Printf("From %s: %s", id, line)
*/
    }
}

func httpStateHandler(w http.ResponseWriter, r *http.Request) {
    id := r.URL.Query().Get("id")
    mu.Lock()
    conn, ok := connections[id]
    mu.Unlock()
    if !ok {
        http.Error(w, "No such UI", 404)
        return
    }
    for _, msg := range conn.Messages {
        fmt.Fprintln(w, msg)
    }
}

func httpSendHandler(w http.ResponseWriter, r *http.Request) {
    id := r.URL.Query().Get("id")
    typ := r.URL.Query().Get("type")
    key := r.URL.Query().Get("key")
    value := r.URL.Query().Get("value")
    cmd := fmt.Sprintf("type|%s||key|%s||value|%s", typ, key, value)
 
    mu.Lock()
    conn, ok := connections[id]
    mu.Unlock()
    if !ok {
        http.Error(w, "No such UI", 404)
        return
    }


    if err := SendMessage(conn.Conn, []byte(cmd)); err != nil {
           http.Error(w, "Send failed", 500)
           return
    }

    fmt.Fprintf(w, "Sent to %s: %s", id, cmd)
}

func init() {
    log.Printf("Madigan init")
    go tcpHandler()

    http.HandleFunc("/madigan-state", httpStateHandler)
    http.HandleFunc("/madigan-send", httpSendHandler)
    log.Printf("Madigan init done")

}
