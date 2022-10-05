package main

import (
	"flag"
	"fmt"
	"os"
	"os/signal"
	"runtime"
	"syscall"

	"go.uber.org/zap"
	"go.uber.org/zap/zapcore"
	"golang.org/x/term"
	"gopkg.in/natefinch/lumberjack.v2"
)

func makeZapLogger(logPath string, debug bool) (*zap.Logger, error) {
	logger := &lumberjack.Logger{
		Filename: logPath,
	}
	c := make(chan os.Signal, 1)
	signal.Notify(c, syscall.SIGHUP)

	go func() {
		for {
			<-c
			logger.Rotate()
		}
	}()

	sink := zapcore.AddSync(logger)
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

	err := os.Mkdir(LogsDir, 0755)
	if err != nil && !os.IsExist(err) {
		_, _ = fmt.Fprintf(os.Stderr, "cannot create logs dir: %v", err)
		return
	}

	logger, err := makeZapLogger(PortodshimLogPath, *debug)
	if err != nil {
		_, _ = fmt.Fprintf(os.Stderr, "cannot create logger: %v", err)
		return
	}
	_ = zap.ReplaceGlobals(logger)

	server, err := NewPortodshimServer(PortodshimSocket)
	if err != nil {
		zap.S().Fatalf("init error: %v", err)
		return
	}

	if err := server.Serve(); err != nil {
		zap.S().Fatalf("serve error: %v", err)
		return
	}
}
