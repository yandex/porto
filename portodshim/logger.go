package main

import (
	"fmt"
	"os"
	"os/signal"
	"sync/atomic"
	"syscall"
	"unsafe"

	"go.uber.org/zap"
	"go.uber.org/zap/zapcore"
	"golang.org/x/term"
)

func makeZapLogger(logPath string, debug bool) (*zap.Logger, error) {
	sink, err := newSink(logPath, syscall.SIGHUP)
	if err != nil {
		return nil, err
	}
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

func newSink(path string, sig ...os.Signal) (zap.Sink, error) {
	c := make(chan os.Signal, 1)
	signal.Notify(c, sig...)

	s := &sink{
		path:     path,
		notifier: c,
	}

	if err := s.reopen(); err != nil {
		return nil, err
	}

	go s.listenToSignal()

	return s, nil
}

type sink struct {
	path     string
	notifier chan os.Signal
	file     unsafe.Pointer
}

func (m *sink) reopen() error {
	file, err := os.OpenFile(m.path, os.O_WRONLY|os.O_APPEND|os.O_CREATE, 0644)
	if err != nil {
		return fmt.Errorf("failed to open log file on %s: %w", m.path, err)
	}
	old := (*os.File)(m.file)
	atomic.StorePointer(&m.file, unsafe.Pointer(file))
	if old != nil {
		if err := old.Close(); err != nil {
			return fmt.Errorf("failed to close old file: %w", err)
		}
	}
	return nil
}

func (m *sink) listenToSignal() {
	for {
		_, ok := <-m.notifier
		if !ok {
			return
		}
		if err := m.reopen(); err != nil {
			// Last chance to signalize about an error
			_, _ = fmt.Fprintf(os.Stderr, "%s", err)
		}
	}
}

func (m *sink) getFile() *os.File {
	return (*os.File)(atomic.LoadPointer(&m.file))
}

func (m *sink) Close() error {
	signal.Stop(m.notifier)
	close(m.notifier)
	return m.getFile().Close()
}

func (m *sink) Write(p []byte) (n int, err error) {
	return m.getFile().Write(p)
}

func (m *sink) Sync() error {
	return m.getFile().Sync()
}
