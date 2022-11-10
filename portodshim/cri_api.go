package main

import (
	"context"
	"unsafe"

	v1 "k8s.io/cri-api/pkg/apis/runtime/v1"
	v1alpha2 "k8s.io/cri-api/pkg/apis/runtime/v1alpha2"
)

type v1PullImageRequestType = *v1.PullImageRequest
type v1alpha2PullImageRequestType = *v1alpha2.PullImageRequest

func RegisterServer(server *PortodshimServer) {
	v1Server := &v1Server{server}
	v1.RegisterRuntimeServiceServer(server.grpcServer, v1Server)
	v1.RegisterImageServiceServer(server.grpcServer, v1Server)
	v1alpha2Server := &v1alpha2Server{server}
	v1alpha2.RegisterRuntimeServiceServer(server.grpcServer, v1alpha2Server)
	v1alpha2.RegisterImageServiceServer(server.grpcServer, v1alpha2Server)
}

type v1Server struct {
	server *PortodshimServer
}

type v1alpha2Server struct {
	server *PortodshimServer
}

// RUNTIME SERVICE INTERFACE v1
func (s *v1Server) Version(ctx context.Context, req *v1.VersionRequest) (*v1.VersionResponse, error) {
	return s.server.runtimeMapper.Version(ctx, req)
}
func (s *v1Server) RunPodSandbox(ctx context.Context, req *v1.RunPodSandboxRequest) (*v1.RunPodSandboxResponse, error) {
	return s.server.runtimeMapper.RunPodSandbox(ctx, req)
}
func (s *v1Server) StopPodSandbox(ctx context.Context, req *v1.StopPodSandboxRequest) (*v1.StopPodSandboxResponse, error) {
	return s.server.runtimeMapper.StopPodSandbox(ctx, req)
}
func (s *v1Server) RemovePodSandbox(ctx context.Context, req *v1.RemovePodSandboxRequest) (*v1.RemovePodSandboxResponse, error) {
	return s.server.runtimeMapper.RemovePodSandbox(ctx, req)
}
func (s *v1Server) PodSandboxStatus(ctx context.Context, req *v1.PodSandboxStatusRequest) (*v1.PodSandboxStatusResponse, error) {
	return s.server.runtimeMapper.PodSandboxStatus(ctx, req)
}
func (s *v1Server) PodSandboxStats(ctx context.Context, req *v1.PodSandboxStatsRequest) (*v1.PodSandboxStatsResponse, error) {
	return s.server.runtimeMapper.PodSandboxStats(ctx, req)
}
func (s *v1Server) ListPodSandbox(ctx context.Context, req *v1.ListPodSandboxRequest) (*v1.ListPodSandboxResponse, error) {
	return s.server.runtimeMapper.ListPodSandbox(ctx, req)
}
func (s *v1Server) CreateContainer(ctx context.Context, req *v1.CreateContainerRequest) (*v1.CreateContainerResponse, error) {
	return s.server.runtimeMapper.CreateContainer(ctx, req)
}
func (s *v1Server) StartContainer(ctx context.Context, req *v1.StartContainerRequest) (*v1.StartContainerResponse, error) {
	return s.server.runtimeMapper.StartContainer(ctx, req)
}
func (s *v1Server) StopContainer(ctx context.Context, req *v1.StopContainerRequest) (*v1.StopContainerResponse, error) {
	return s.server.runtimeMapper.StopContainer(ctx, req)
}
func (s *v1Server) RemoveContainer(ctx context.Context, req *v1.RemoveContainerRequest) (*v1.RemoveContainerResponse, error) {
	return s.server.runtimeMapper.RemoveContainer(ctx, req)
}
func (s *v1Server) ListContainers(ctx context.Context, req *v1.ListContainersRequest) (*v1.ListContainersResponse, error) {
	return s.server.runtimeMapper.ListContainers(ctx, req)
}
func (s *v1Server) ContainerStatus(ctx context.Context, req *v1.ContainerStatusRequest) (*v1.ContainerStatusResponse, error) {
	return s.server.runtimeMapper.ContainerStatus(ctx, req)
}
func (s *v1Server) UpdateContainerResources(ctx context.Context, req *v1.UpdateContainerResourcesRequest) (*v1.UpdateContainerResourcesResponse, error) {
	return s.server.runtimeMapper.UpdateContainerResources(ctx, req)
}
func (s *v1Server) ReopenContainerLog(ctx context.Context, req *v1.ReopenContainerLogRequest) (*v1.ReopenContainerLogResponse, error) {
	return s.server.runtimeMapper.ReopenContainerLog(ctx, req)
}
func (s *v1Server) ExecSync(ctx context.Context, req *v1.ExecSyncRequest) (*v1.ExecSyncResponse, error) {
	return s.server.runtimeMapper.ExecSync(ctx, req)
}
func (s *v1Server) Exec(ctx context.Context, req *v1.ExecRequest) (*v1.ExecResponse, error) {
	return s.server.runtimeMapper.Exec(ctx, req)
}
func (s *v1Server) Attach(ctx context.Context, req *v1.AttachRequest) (*v1.AttachResponse, error) {
	return s.server.runtimeMapper.Attach(ctx, req)
}
func (s *v1Server) PortForward(ctx context.Context, req *v1.PortForwardRequest) (*v1.PortForwardResponse, error) {
	return s.server.runtimeMapper.PortForward(ctx, req)
}
func (s *v1Server) ContainerStats(ctx context.Context, req *v1.ContainerStatsRequest) (*v1.ContainerStatsResponse, error) {
	return s.server.runtimeMapper.ContainerStats(ctx, req)
}
func (s *v1Server) ListContainerStats(ctx context.Context, req *v1.ListContainerStatsRequest) (*v1.ListContainerStatsResponse, error) {
	return s.server.runtimeMapper.ListContainerStats(ctx, req)
}
func (s *v1Server) ListPodSandboxStats(ctx context.Context, req *v1.ListPodSandboxStatsRequest) (*v1.ListPodSandboxStatsResponse, error) {
	return s.server.runtimeMapper.ListPodSandboxStats(ctx, req)
}
func (s *v1Server) UpdateRuntimeConfig(ctx context.Context, req *v1.UpdateRuntimeConfigRequest) (*v1.UpdateRuntimeConfigResponse, error) {
	return s.server.runtimeMapper.UpdateRuntimeConfig(ctx, req)
}
func (s *v1Server) Status(ctx context.Context, req *v1.StatusRequest) (*v1.StatusResponse, error) {
	return s.server.runtimeMapper.Status(ctx, req)
}

// RUNTIME SERVICE INTERFACE v1alpha2
func (s *v1alpha2Server) Version(ctx context.Context, req *v1alpha2.VersionRequest) (*v1alpha2.VersionResponse, error) {
	resp, err := s.server.runtimeMapper.Version(ctx, (*v1.VersionRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.VersionResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) RunPodSandbox(ctx context.Context, req *v1alpha2.RunPodSandboxRequest) (*v1alpha2.RunPodSandboxResponse, error) {
	resp, err := s.server.runtimeMapper.RunPodSandbox(ctx, (*v1.RunPodSandboxRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.RunPodSandboxResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) StopPodSandbox(ctx context.Context, req *v1alpha2.StopPodSandboxRequest) (*v1alpha2.StopPodSandboxResponse, error) {
	resp, err := s.server.runtimeMapper.StopPodSandbox(ctx, (*v1.StopPodSandboxRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.StopPodSandboxResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) RemovePodSandbox(ctx context.Context, req *v1alpha2.RemovePodSandboxRequest) (*v1alpha2.RemovePodSandboxResponse, error) {
	resp, err := s.server.runtimeMapper.RemovePodSandbox(ctx, (*v1.RemovePodSandboxRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.RemovePodSandboxResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) PodSandboxStatus(ctx context.Context, req *v1alpha2.PodSandboxStatusRequest) (*v1alpha2.PodSandboxStatusResponse, error) {
	resp, err := s.server.runtimeMapper.PodSandboxStatus(ctx, (*v1.PodSandboxStatusRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.PodSandboxStatusResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) PodSandboxStats(ctx context.Context, req *v1alpha2.PodSandboxStatsRequest) (*v1alpha2.PodSandboxStatsResponse, error) {
	resp, err := s.server.runtimeMapper.PodSandboxStats(ctx, (*v1.PodSandboxStatsRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.PodSandboxStatsResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) ListPodSandbox(ctx context.Context, req *v1alpha2.ListPodSandboxRequest) (*v1alpha2.ListPodSandboxResponse, error) {
	resp, err := s.server.runtimeMapper.ListPodSandbox(ctx, (*v1.ListPodSandboxRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.ListPodSandboxResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) CreateContainer(ctx context.Context, req *v1alpha2.CreateContainerRequest) (*v1alpha2.CreateContainerResponse, error) {
	resp, err := s.server.runtimeMapper.CreateContainer(ctx, (*v1.CreateContainerRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.CreateContainerResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) StartContainer(ctx context.Context, req *v1alpha2.StartContainerRequest) (*v1alpha2.StartContainerResponse, error) {
	resp, err := s.server.runtimeMapper.StartContainer(ctx, (*v1.StartContainerRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.StartContainerResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) StopContainer(ctx context.Context, req *v1alpha2.StopContainerRequest) (*v1alpha2.StopContainerResponse, error) {
	resp, err := s.server.runtimeMapper.StopContainer(ctx, (*v1.StopContainerRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.StopContainerResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) RemoveContainer(ctx context.Context, req *v1alpha2.RemoveContainerRequest) (*v1alpha2.RemoveContainerResponse, error) {
	resp, err := s.server.runtimeMapper.RemoveContainer(ctx, (*v1.RemoveContainerRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.RemoveContainerResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) ListContainers(ctx context.Context, req *v1alpha2.ListContainersRequest) (*v1alpha2.ListContainersResponse, error) {
	resp, err := s.server.runtimeMapper.ListContainers(ctx, (*v1.ListContainersRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.ListContainersResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) ContainerStatus(ctx context.Context, req *v1alpha2.ContainerStatusRequest) (*v1alpha2.ContainerStatusResponse, error) {
	resp, err := s.server.runtimeMapper.ContainerStatus(ctx, (*v1.ContainerStatusRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.ContainerStatusResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) UpdateContainerResources(ctx context.Context, req *v1alpha2.UpdateContainerResourcesRequest) (*v1alpha2.UpdateContainerResourcesResponse, error) {
	resp, err := s.server.runtimeMapper.UpdateContainerResources(ctx, (*v1.UpdateContainerResourcesRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.UpdateContainerResourcesResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) ReopenContainerLog(ctx context.Context, req *v1alpha2.ReopenContainerLogRequest) (*v1alpha2.ReopenContainerLogResponse, error) {
	resp, err := s.server.runtimeMapper.ReopenContainerLog(ctx, (*v1.ReopenContainerLogRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.ReopenContainerLogResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) ExecSync(ctx context.Context, req *v1alpha2.ExecSyncRequest) (*v1alpha2.ExecSyncResponse, error) {
	resp, err := s.server.runtimeMapper.ExecSync(ctx, (*v1.ExecSyncRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.ExecSyncResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) Exec(ctx context.Context, req *v1alpha2.ExecRequest) (*v1alpha2.ExecResponse, error) {
	resp, err := s.server.runtimeMapper.Exec(ctx, (*v1.ExecRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.ExecResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) Attach(ctx context.Context, req *v1alpha2.AttachRequest) (*v1alpha2.AttachResponse, error) {
	resp, err := s.server.runtimeMapper.Attach(ctx, (*v1.AttachRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.AttachResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) PortForward(ctx context.Context, req *v1alpha2.PortForwardRequest) (*v1alpha2.PortForwardResponse, error) {
	resp, err := s.server.runtimeMapper.PortForward(ctx, (*v1.PortForwardRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.PortForwardResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) ContainerStats(ctx context.Context, req *v1alpha2.ContainerStatsRequest) (*v1alpha2.ContainerStatsResponse, error) {
	resp, err := s.server.runtimeMapper.ContainerStats(ctx, (*v1.ContainerStatsRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.ContainerStatsResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) ListContainerStats(ctx context.Context, req *v1alpha2.ListContainerStatsRequest) (*v1alpha2.ListContainerStatsResponse, error) {
	resp, err := s.server.runtimeMapper.ListContainerStats(ctx, (*v1.ListContainerStatsRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.ListContainerStatsResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) ListPodSandboxStats(ctx context.Context, req *v1alpha2.ListPodSandboxStatsRequest) (*v1alpha2.ListPodSandboxStatsResponse, error) {
	resp, err := s.server.runtimeMapper.ListPodSandboxStats(ctx, (*v1.ListPodSandboxStatsRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.ListPodSandboxStatsResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) UpdateRuntimeConfig(ctx context.Context, req *v1alpha2.UpdateRuntimeConfigRequest) (*v1alpha2.UpdateRuntimeConfigResponse, error) {
	resp, err := s.server.runtimeMapper.UpdateRuntimeConfig(ctx, (*v1.UpdateRuntimeConfigRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.UpdateRuntimeConfigResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) Status(ctx context.Context, req *v1alpha2.StatusRequest) (*v1alpha2.StatusResponse, error) {
	resp, err := s.server.runtimeMapper.Status(ctx, (*v1.StatusRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.StatusResponse)(unsafe.Pointer(resp)), err
}

// IMAGE SERVICE INTERFACE v1
func (s *v1Server) ListImages(ctx context.Context, req *v1.ListImagesRequest) (*v1.ListImagesResponse, error) {
	return s.server.imageMapper.ListImages(ctx, req)
}
func (s *v1Server) ImageStatus(ctx context.Context, req *v1.ImageStatusRequest) (*v1.ImageStatusResponse, error) {
	return s.server.imageMapper.ImageStatus(ctx, req)
}
func (s *v1Server) PullImage(ctx context.Context, req *v1.PullImageRequest) (*v1.PullImageResponse, error) {
	return s.server.imageMapper.PullImage(ctx, req)
}
func (s *v1Server) RemoveImage(ctx context.Context, req *v1.RemoveImageRequest) (*v1.RemoveImageResponse, error) {
	return s.server.imageMapper.RemoveImage(ctx, req)
}
func (s *v1Server) ImageFsInfo(ctx context.Context, req *v1.ImageFsInfoRequest) (*v1.ImageFsInfoResponse, error) {
	return s.server.imageMapper.ImageFsInfo(ctx, req)
}

// IMAGE SERVICE INTERFACE v1alpha2
func (s *v1alpha2Server) ListImages(ctx context.Context, req *v1alpha2.ListImagesRequest) (*v1alpha2.ListImagesResponse, error) {
	resp, err := s.server.imageMapper.ListImages(ctx, (*v1.ListImagesRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.ListImagesResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) ImageStatus(ctx context.Context, req *v1alpha2.ImageStatusRequest) (*v1alpha2.ImageStatusResponse, error) {
	resp, err := s.server.imageMapper.ImageStatus(ctx, (*v1.ImageStatusRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.ImageStatusResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) PullImage(ctx context.Context, req *v1alpha2.PullImageRequest) (*v1alpha2.PullImageResponse, error) {
	resp, err := s.server.imageMapper.PullImage(ctx, (*v1.PullImageRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.PullImageResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) RemoveImage(ctx context.Context, req *v1alpha2.RemoveImageRequest) (*v1alpha2.RemoveImageResponse, error) {
	resp, err := s.server.imageMapper.RemoveImage(ctx, (*v1.RemoveImageRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.RemoveImageResponse)(unsafe.Pointer(resp)), err
}
func (s *v1alpha2Server) ImageFsInfo(ctx context.Context, req *v1alpha2.ImageFsInfoRequest) (*v1alpha2.ImageFsInfoResponse, error) {
	resp, err := s.server.imageMapper.ImageFsInfo(ctx, (*v1.ImageFsInfoRequest)(unsafe.Pointer(req)))
	return (*v1alpha2.ImageFsInfoResponse)(unsafe.Pointer(resp)), err
}
