package main

import (
//    "bufio"
    "log"
    "net/http"
//    "strconv"


//	"time"

)


func main() {
//    go tcpHandler()

//    http.HandleFunc("/state", httpStateHandler)
//    http.HandleFunc("/send", httpSendHandler)

    log.Println("HTTP server on :8080")
    log.Fatal(http.ListenAndServe(":8080", nil))
}
