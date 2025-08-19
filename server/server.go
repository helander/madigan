// =====================================================================================================
// File:           server.go
// Project:        madigan
// Author:         Lars-Erik Helander <lehswel@gmail.com>
// License:        MIT
// Description:    Web server with embedded static content
// =====================================================================================================

package main

import (
	"context"
	"embed"
	"fmt"
	"io/fs"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"
)

// =====================================================================================================
// Types & constants
// =====================================================================================================

// =====================================================================================================
// Local state
// =====================================================================================================
//go:embed embed/* 
var embeddedFiles embed.FS
/**/


// =====================================================================================================
// Local functions
// =====================================================================================================

// =====================================================================================================
// Main
// =====================================================================================================
func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	// Embedded static files
	staticFS, _ := fs.Sub(embeddedFiles, "embed")
	http.Handle("/", http.FileServer(http.FS(staticFS)))

	// Non-embedded static files
        localFS := http.FileServer(http.Dir("./local"))
        http.Handle("/local/", http.StripPrefix("/local/", localFS))

	port := 17000

	server := &http.Server{
		Addr:         fmt.Sprintf(":%d", port),
		ReadTimeout:  5 * time.Second,
		WriteTimeout: 10 * time.Second,
		IdleTimeout:  120 * time.Second,
	}


	go func() {
		log.Printf("LV2_PATH=%s", os.Getenv("LV2_PATH"))
		log.Printf("Server started at http://localhost:%d", port)
		if err := server.ListenAndServe(); err != http.ErrServerClosed {
			log.Fatal(err)
		}
	}()


	// Wait for signal (Ctrl-C or SIGTERM)
	<-ctx.Done()
	log.Println("Terminating...")

	// Clean server shutdown
	server.Shutdown(context.Background())

}

