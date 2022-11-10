package main

import (
	"context"
	"fmt"
	"net"
	"os"
	"os/signal"
	"strings"
	"time"

	"math/rand"

	"github.com/yandex/porto/src/api/go/porto"
	"go.uber.org/zap"
	"golang.org/x/sys/unix"
	grpc "google.golang.org/grpc"
)

type PortodshimServer struct {
	socket        string
	listener      net.Listener
	grpcServer    *grpc.Server
	runtimeMapper *PortodshimRuntimeMapper
	imageMapper   *PortodshimImageMapper
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

func portoClientContext(ctx context.Context) (context.Context, error) {
	portoClient, err := porto.Connect()
	if err != nil {
		return ctx, fmt.Errorf("connect to porto: %v", err)
	}

	c := ctx
	c = context.WithValue(c, "requestId", fmt.Sprintf("%08x", rand.Intn(4294967296)))
	c = context.WithValue(c, "portoClient", portoClient)
	return c, nil
}

func getPortoClient(ctx context.Context) porto.API {
	return ctx.Value("portoClient").(porto.API)
}

func getRequestId(ctx context.Context) string {
	return ctx.Value("requestId").(string)
}

func serverInterceptor(ctx context.Context,
	req interface{},
	info *grpc.UnaryServerInfo,
	handler grpc.UnaryHandler) (interface{}, error) {
	start := time.Now()

	ctx, err := portoClientContext(ctx)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	startTime := time.Since(start).Milliseconds()

	InfoLog(ctx, "%s", info.FullMethod)
	if !strings.Contains(info.FullMethod, "PullImage") {
		DebugLog(ctx, "%s", req)
	} else {
		if strings.Contains(info.FullMethod, "v1alpha2") {
			reqPullImage := req.(v1alpha2PullImageRequestType)
			DebugLog(ctx, "%s %s", reqPullImage.GetImage(), reqPullImage.GetAuth().GetUsername())
		} else {
			reqPullImage := req.(v1PullImageRequestType)
			DebugLog(ctx, "%s %s", reqPullImage.GetImage(), reqPullImage.GetAuth().GetUsername())
		}
	}

	h, err := handler(ctx, req)

	DebugLog(ctx, "%+v", h)
	InfoLog(ctx, "%s time: %d ms", info.FullMethod, time.Since(start).Milliseconds()-startTime)

	return h, err
}

func NewPortodshimServer(socketPath string) (*PortodshimServer, error) {
	var err error
	zap.S().Info("starting of portodshim initialization")

	server := PortodshimServer{socket: socketPath}
	server.ctx = server.ShutdownCtx()

	server.runtimeMapper, err = NewPortodshimRuntimeMapper()
	if err != nil {
		return nil, err
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

	server.grpcServer = grpc.NewServer(grpc.UnaryInterceptor(serverInterceptor))
	RegisterServer(&server)

	rand.Seed(time.Now().UnixNano())

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
