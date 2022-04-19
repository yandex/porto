package main

import (
	"context"
	"fmt"
	"net"
	"os"
	"os/signal"
	"time"

	"go.uber.org/zap"
	"golang.org/x/sys/unix"
	grpc "google.golang.org/grpc"
	v1alpha2 "k8s.io/cri-api/pkg/apis/runtime/v1alpha2"
)

type PortodshimServer struct {
	socket        string
	listener      net.Listener
	grpcServer    *grpc.Server
	runtimeMapper PortodshimRuntimeMapper
	imageMapper   PortodshimImageMapper
	ctx           context.Context
}

func unlinkStaleSocket(socketPath string) error {
	ss, err := os.Stat(socketPath)
	if err == nil {
		if ss.Mode()&os.ModeSocket != 0 {
			err = os.Remove(socketPath)
			if err != nil {
				return fmt.Errorf("failed to remove socket: %v", err)
			}
			zap.S().Info("unlinked staled socket")
			return nil
		} else {
			return fmt.Errorf("some file already exists at path %q and isn't a socket", socketPath)
		}
	} else {
		if os.IsNotExist(err) {
			return nil
		} else {
			return fmt.Errorf("failed to stat socket path: %v", err)
		}
	}
}

func serverInterceptor(ctx context.Context,
	req interface{},
	info *grpc.UnaryServerInfo,
	handler grpc.UnaryHandler) (interface{}, error) {
	start := time.Now()

	portoClient, err := Connect()
	if err != nil {
		zap.S().Fatalf("connect to porto: %v", err)
		return nil, fmt.Errorf("connect to porto: %v", err)
	}
	defer portoClient.Close()

	h, err := handler(context.WithValue(ctx, "portoClient", portoClient), req)
	zap.S().Debugf("request: %s\tduration: %s\terror: %v\n", info.FullMethod, time.Since(start), err)

	return h, err
}

func NewPortodshimServer(socketPath string) (*PortodshimServer, error) {
	var err error
	zap.S().Info("starting of portodshim initialization")

	server := PortodshimServer{socket: socketPath}
	server.ctx = server.ShutdownCtx()

	server.runtimeMapper.containerStateMap = map[string]v1alpha2.ContainerState{
		"stopped":    v1alpha2.ContainerState_CONTAINER_CREATED,
		"paused":     v1alpha2.ContainerState_CONTAINER_CREATED,
		"starting":   v1alpha2.ContainerState_CONTAINER_RUNNING,
		"running":    v1alpha2.ContainerState_CONTAINER_RUNNING,
		"stopping":   v1alpha2.ContainerState_CONTAINER_RUNNING,
		"respawning": v1alpha2.ContainerState_CONTAINER_RUNNING,
		"meta":       v1alpha2.ContainerState_CONTAINER_RUNNING,
		"dead":       v1alpha2.ContainerState_CONTAINER_EXITED,
	}
	server.runtimeMapper.podStateMap = map[string]v1alpha2.PodSandboxState{
		"stopped":    v1alpha2.PodSandboxState_SANDBOX_NOTREADY,
		"paused":     v1alpha2.PodSandboxState_SANDBOX_NOTREADY,
		"starting":   v1alpha2.PodSandboxState_SANDBOX_NOTREADY,
		"running":    v1alpha2.PodSandboxState_SANDBOX_READY,
		"stopping":   v1alpha2.PodSandboxState_SANDBOX_NOTREADY,
		"respawning": v1alpha2.PodSandboxState_SANDBOX_NOTREADY,
		"meta":       v1alpha2.PodSandboxState_SANDBOX_READY,
		"dead":       v1alpha2.PodSandboxState_SANDBOX_NOTREADY,
	}

	err = unlinkStaleSocket(socketPath)
	if err != nil {
		zap.S().Fatalf("failed to unlink staled socket: %v", err)
	}
	server.listener, err = net.Listen("unix", server.socket)
	if err != nil {
		zap.S().Fatalf("listen error: %s %v", server.socket, err)
		return nil, fmt.Errorf("listen error: %s %v", server.socket, err)
	}

	if err = os.Chmod(server.socket, 0o660); err != nil {
		zap.S().Warnf("chmod error: %s %v", server.socket, err)
		return nil, fmt.Errorf("chmod error: %s %v", server.socket, err)
	}

	err = os.Mkdir(VolumesPath, 0755)
	if err != nil && !os.IsExist(err) {
		return nil, err
	}

	server.grpcServer = grpc.NewServer(grpc.UnaryInterceptor(serverInterceptor))

	// TODO: Добавить другую версию API
	v1alpha2.RegisterRuntimeServiceServer(server.grpcServer, &server.runtimeMapper)
	v1alpha2.RegisterImageServiceServer(server.grpcServer, &server.imageMapper)

	zap.S().Info("portodshim is initialized")
	return &server, nil
}

func (server *PortodshimServer) Serve() error {
	zap.S().Info("starting of portodshim serving")

	go func() {
		if err := server.grpcServer.Serve(server.listener); err != nil {
			zap.S().Fatalf("unable to run GRPC server: %v", err)
		}
	}()

	zap.S().Info("portodshim is serving...")

	<-server.ctx.Done()
	server.Shutdown()

	zap.S().Info("portodshim is shut down")
	return nil
}

func (server *PortodshimServer) Shutdown() {
	zap.S().Info("portodshim is shutting down")

	server.grpcServer.GracefulStop()
}

func (server *PortodshimServer) ShutdownCtx() (ctx context.Context) {
	sig := make(chan os.Signal)
	signal.Notify(sig, unix.SIGINT, unix.SIGTERM)
	ctx, cancel := context.WithCancel(context.Background())
	go func() {
		<-sig
		cancel()
	}()
	return
}
