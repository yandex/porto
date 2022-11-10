package main

import (
	"fmt"
	"io"
	"os"
	"time"

	term "github.com/creack/pty"
	"go.uber.org/zap"
	"golang.org/x/net/context"
	remotecommandconsts "k8s.io/apimachinery/pkg/util/remotecommand"
	"k8s.io/client-go/tools/remotecommand"
	"k8s.io/kubernetes/pkg/kubelet/cri/streaming"
)

func NewStreamingServer(addr string) (streaming.Server, error) {
	config := streaming.Config{
		Addr:                            addr,
		StreamIdleTimeout:               15 * time.Minute,
		StreamCreationTimeout:           remotecommandconsts.DefaultStreamCreationTimeout,
		SupportedRemoteCommandProtocols: remotecommandconsts.SupportedStreamingProtocols,
	}
	runtime, _ := NewStreamingRuntime()
	return streaming.NewServer(config, runtime)
}

func NewStreamingRuntime() (StreamingRuntime, error) {
	return StreamingRuntime{}, nil
}

type StreamingRuntime struct{}

func (sr StreamingRuntime) Exec(containerID string, cmd []string, stdin io.Reader, stdout, stderr io.WriteCloser, terminal bool, resize <-chan remotecommand.TerminalSize) error {
	ctx, err := portoClientContext(context.Background())
	if err != nil {
		return fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	pc := getPortoClient(ctx)

	id := containerID + createId("/exec")

	if err := pc.CreateWeak(id); err != nil {
		return fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	if err := prepareContainerCommand(ctx, id, cmd, nil, nil, nil, true); err != nil {
		return fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	if err := pc.SetProperty(id, "isolate", "false"); err != nil {
		return fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	if err := pc.SetProperty(id, "net", "inherited"); err != nil {
		return fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	var (
		stdin_r, stdin_w   *os.File
		stdout_r, stdout_w *os.File
		stderr_r, stderr_w *os.File
		tty, pty           *os.File
	)

	if terminal {
		tty, pty, err = term.Open()
		if err != nil {
			return err
		}
		defer tty.Close()
		defer pty.Close()
	}

	if stdin != nil {
		if terminal {
			stdin_r = pty
			stdin_w = tty
		} else {
			stdin_r, stdin_w, err = os.Pipe()
			if err != nil {
				return err
			}
			defer stdin_r.Close()
			defer stdin_w.Close()
		}

		if err := pc.SetProperty(id, "stdin_path", fmt.Sprintf("/dev/fd/%d", stdin_r.Fd())); err != nil {
			return fmt.Errorf("%s: %v", getCurrentFuncName(), err)
		}
	}

	if stdout != nil {
		if terminal {
			stdout_r = tty
			stdout_w = pty
		} else {
			stdout_r, stdout_w, err = os.Pipe()
			if err != nil {
				return err
			}
			defer stdout_r.Close()
			defer stdout_w.Close()
		}

		if err := pc.SetProperty(id, "stdout_path", fmt.Sprintf("/dev/fd/%d", stdout_w.Fd())); err != nil {
			return fmt.Errorf("%s: %v", getCurrentFuncName(), err)
		}
	}

	if terminal {
		stderr_r = tty
		stderr_w = pty
	} else if stderr != nil {
		stderr_r, stderr_w, err = os.Pipe()
		if err != nil {
			return err
		}
		defer stderr_r.Close()
		defer stderr_w.Close()
	}

	if stderr_w != nil {
		if err := pc.SetProperty(id, "stderr_path", fmt.Sprintf("/dev/fd/%d", stderr_w.Fd())); err != nil {
			return fmt.Errorf("%s: %v", getCurrentFuncName(), err)
		}
	}

	if err := pc.Start(id); err != nil {
		return fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	if !terminal {
		if stdin != nil {
			stdin_r.Close()
		}
		if stdout != nil {
			stdout_w.Close()
		}
		if stderr != nil {
			stderr_w.Close()
		}
	}

	ctx, cancel := context.WithCancel(ctx)

	copyStream := func(ctx context.Context, dst io.Writer, src io.Reader, name string) {
		for {
			select {
			case <-ctx.Done():
				return
			default:
				n, err := io.Copy(dst, src)
				if err != nil {
					zap.S().Warnf("cannot copy %s: %v", name, err)
					return
				}
				if n == 0 {
					return
				}
			}

		}
	}

	if stdin != nil {
		go copyStream(ctx, stdin_w, stdin, "stdin")
	}
	if stdout != nil {
		go copyStream(ctx, stdout, stdout_r, "stdout")
	}
	if stderr != nil {
		go copyStream(ctx, stderr, stderr_r, "stderr")
	}

	defer cancel()
	_, err = pc.Wait([]string{id}, -1)
	if err != nil {
		zap.S().Warnf("failed to wait %s: %v", id, err)
	}

	return nil
}
func (sr StreamingRuntime) Attach(containerID string, in io.Reader, out, err io.WriteCloser, tty bool, resize <-chan remotecommand.TerminalSize) error {
	return nil
}
func (sr StreamingRuntime) PortForward(podSandboxID string, port int32, stream io.ReadWriteCloser) error {
	return nil
}
