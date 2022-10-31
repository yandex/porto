package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"io"
	"log"
	"os"
	"os/exec"
	"runtime"
	"strings"
	"sync"
	"syscall"
	"time"
)

type logEnt struct {
	Log    string    `json:"log"`
	Stream string    `json:"stream"`
	Time   time.Time `json:"time"`
}

func syncWrite(mtx *sync.Mutex, w io.Writer, buf []byte) (int64, error) {
	mtx.Lock()
	defer mtx.Unlock()
	r1 := bytes.NewReader(buf)
	r2 := bytes.NewReader([]byte{'\n'})
	return io.Copy(w, io.MultiReader(r1, r2))
}

func streamLoop(mtx *sync.Mutex, w io.Writer, stream io.Reader, streamName string) {
	scanner := bufio.NewScanner(stream)
	logLine := &logEnt{Stream: streamName}
	for scanner.Scan() {
		logLine.Log = scanner.Text() + "\n"
		logLine.Time = time.Now()
		buf, err := json.Marshal(&logLine)
		if err != nil {
			log.Fatalf("failed to marshal json: %v", err)
		}
		_, err = syncWrite(mtx, w, buf)
		if err != nil {
			log.Fatalf("failed to write stream %s log: %v", streamName, err)
		}
	}
}

func main() {
	// Don't need more threads, so set limit explicitly.
	runtime.GOMAXPROCS(2)

	// At least we need a command here
	if len(os.Args) < 2 {
		os.Exit(-1)
	}

	realCmd := os.Args[1]
	realArgs := os.Args[2:]
	cmd := exec.Command(realCmd, realArgs...)
	cmd.SysProcAttr = &syscall.SysProcAttr{
		Pdeathsig: syscall.SIGKILL,
	}

	stderr, err := cmd.StderrPipe()
	if err != nil {
		log.Fatalf("failed to create stderr pipe: %v", err)
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		log.Fatalf("failed to create stdout pipe: %v", err)
	}

	logMtx := sync.Mutex{}
	if err := cmd.Start(); err != nil {
		log.Fatalf("failed to start command '%s' with args '%s': %v", realCmd, strings.Join(realArgs, " "), err)
	}

	// Let's write real logs to stdout
	go streamLoop(&logMtx, os.Stdout, stderr, "stderr")
	go streamLoop(&logMtx, os.Stdout, stdout, "stdout")
	err = cmd.Wait()
	if err != nil {
		if err, ok := err.(*exec.ExitError); ok {
			os.Exit(err.ExitCode())
		}
		log.Fatalf("failed to wait command '%s' with args '%s': %v", realCmd, realArgs, err)
	}
}
