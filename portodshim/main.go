package main

import (
	"flag"
	"fmt"
	"os"
	"runtime"

	"go.uber.org/zap"
	"go.uber.org/zap/zapcore"
	"golang.org/x/term"
	"gopkg.in/natefinch/lumberjack.v2"
)

const (
	portodshimSocket  = "/run/portodshim.sock"
	portodshimLogPath = "/var/log/portodshim.log"
)

func makeZapLogger(logPath string, debug bool) (*zap.Logger, error) {
	sink := zapcore.AddSync(&lumberjack.Logger{
		Filename:   logPath,
		MaxSize:    500, // megabytes
		MaxBackups: 3,
		MaxAge:     28, // days
	})
	encoderCfg := zap.NewProductionEncoderConfig()
	encoderCfg.EncodeTime = zapcore.ISO8601TimeEncoder
	encoderCfg.EncodeLevel = zapcore.CapitalLevelEncoder
	encoder := zapcore.NewConsoleEncoder(encoderCfg)
	al := zap.NewAtomicLevelAt(zapcore.InfoLevel)
	if debug {
		al = zap.NewAtomicLevelAt(zapcore.DebugLevel)
	}
	core := zapcore.NewCore(encoder, sink, al)
	if term.IsTerminal(int(os.Stdout.Fd())) {
		core = zapcore.NewTee(
			core,
			zapcore.NewCore(encoder, zapcore.Lock(os.Stdout), al))
	}
	return zap.New(core), nil
}

func getCurrentFuncName() string {
	pc, _, _, _ := runtime.Caller(1)
	return runtime.FuncForPC(pc).Name()
}

func main() {
	debug := flag.Bool("debug", false, "show debug logs")
	flag.Parse()

	logger, err := makeZapLogger(portodshimLogPath, *debug)
	if err != nil {
		_, _ = fmt.Fprintf(os.Stderr, "cannot create logger: %v", err)
		return
	}
	_ = zap.ReplaceGlobals(logger)

	server, err := NewPortodshimServer(portodshimSocket)
	if err != nil {
		zap.S().Fatalf("init error: %v", err)
		return
	}

	if err := server.Serve(); err != nil {
		zap.S().Fatalf("serve error: %v", err)
		return
	}
}
