package main

import (
	"flag"
	"fmt"
	"os"
	"runtime"

	"go.uber.org/zap"
)

func getCurrentFuncName() string {
	pc, _, _, _ := runtime.Caller(1)
	return runtime.FuncForPC(pc).Name()
}

func main() {
	debug := flag.Bool("debug", false, "show debug logs")
	flag.Parse()

	err := os.Mkdir(LogsDir, 0755)
	if err != nil && !os.IsExist(err) {
		_, _ = fmt.Fprintf(os.Stderr, "cannot create logs dir: %v", err)
		return
	}

	err = CreateZapLogger(*debug)
	if err != nil {
		_, _ = fmt.Fprintf(os.Stderr, "%v", err)
		return
	}

	err = os.Mkdir(VolumesDir, 0755)
	if err != nil && !os.IsExist(err) {
		zap.S().Fatalf("cannot create volumes dir: %v", err)
		return
	}

	err = InitKnownRegistries()
	if err != nil {
		zap.S().Fatalf("cannot init known registries: %v", err)
		return
	}

	server, err := NewPortodshimServer(PortodshimSocket)
	if err != nil {
		zap.S().Fatalf("server init error: %v", err)
		return
	}

	if err := server.Serve(); err != nil {
		zap.S().Fatalf("serve error: %v", err)
		return
	}
}
