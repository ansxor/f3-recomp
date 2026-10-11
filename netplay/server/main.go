package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"
)

const usageHeader = `Usage: netplay-server [flags]

Two-player netplay relay (protocol v1). Wire format:
docs/site/developer/netplay/protocol.md

Flags:
`

func main() {
	fs := flag.NewFlagSet("netplay-server", flag.ExitOnError)
	tcpAddr := fs.String("tcp", ":7845", "TCP listen address for the control connection")
	udpAddr := fs.String("udp", ":7845", "UDP listen address for input packets")
	logDir := fs.String("logdir", "./netplay-logs", "directory for per-session JSON logs")
	maxRooms := fs.Int("max-rooms", 64, "maximum number of rooms and active sessions")
	fs.Usage = func() {
		fmt.Fprint(fs.Output(), usageHeader)
		fs.PrintDefaults()
	}
	fs.Parse(os.Args[1:])

	if *maxRooms < 1 {
		fmt.Fprintln(os.Stderr, "netplay-server: -max-rooms must be at least 1")
		os.Exit(2)
	}

	srv, err := New(Config{
		TCPAddr:  *tcpAddr,
		UDPAddr:  *udpAddr,
		LogDir:   *logDir,
		MaxRooms: *maxRooms,
	})
	if err != nil {
		log.Fatalf("netplay-server: %v", err)
	}
	log.Printf("netplay-server: tcp %s, udp %s, logdir %s, max-rooms %d",
		srv.TCPAddr(), srv.UDPAddr(), *logDir, *maxRooms)

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	if err := srv.Serve(ctx); err != nil {
		log.Fatalf("netplay-server: %v", err)
	}
}
