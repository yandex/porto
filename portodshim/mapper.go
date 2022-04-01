package main

import (
	"context"
	"fmt"
	v1alpha2 "k8s.io/cri-api/pkg/apis/runtime/v1alpha2"
	"strconv"
	"time"
)

const (
	APIVersion    = "0.1.0"
	containerName = "porto"
)

type PortodshimMapper struct {
	portoClient API
	stateMap    map[string]v1alpha2.ContainerState
}

func (mapper *PortodshimMapper) Version(ctx context.Context, req *v1alpha2.VersionRequest) (*v1alpha2.VersionResponse, error) {
	tag, rev, err := mapper.portoClient.GetVersion()
	if err != nil {
		return nil, fmt.Errorf("error in PortodshimMapper.Version(): %v", err)
	}
	return &v1alpha2.VersionResponse{
		Version:           APIVersion,
		RuntimeName:       containerName,
		RuntimeVersion:    tag,
		RuntimeApiVersion: rev,
	}, nil
}
func (mapper *PortodshimMapper) RunPodSandbox(ctx context.Context, req *v1alpha2.RunPodSandboxRequest) (*v1alpha2.RunPodSandboxResponse, error) {
	return nil, fmt.Errorf("not implemented RunPodSandbox")
}
func (mapper *PortodshimMapper) StopPodSandbox(ctx context.Context, req *v1alpha2.StopPodSandboxRequest) (*v1alpha2.StopPodSandboxResponse, error) {
	return nil, fmt.Errorf("not implemented StopPodSandbox")
}
func (mapper *PortodshimMapper) RemovePodSandbox(ctx context.Context, req *v1alpha2.RemovePodSandboxRequest) (*v1alpha2.RemovePodSandboxResponse, error) {
	return nil, fmt.Errorf("not implemented RemovePodSandbox")
}
func (mapper *PortodshimMapper) PodSandboxStatus(ctx context.Context, req *v1alpha2.PodSandboxStatusRequest) (*v1alpha2.PodSandboxStatusResponse, error) {
	return nil, fmt.Errorf("not implemented PodSandboxStatus")
}
func (mapper *PortodshimMapper) PodSandboxStats(ctx context.Context, req *v1alpha2.PodSandboxStatsRequest) (*v1alpha2.PodSandboxStatsResponse, error) {
	return nil, fmt.Errorf("not implemented PodSandboxStats")
}
func (mapper *PortodshimMapper) ListPodSandbox(ctx context.Context, req *v1alpha2.ListPodSandboxRequest) (*v1alpha2.ListPodSandboxResponse, error) {
	return nil, fmt.Errorf("not implemented ListPodSandbox")
}
func (mapper *PortodshimMapper) CreateContainer(ctx context.Context, req *v1alpha2.CreateContainerRequest) (*v1alpha2.CreateContainerResponse, error) {
	name := req.Config.Metadata.Name
	err := mapper.portoClient.Create(name)
	if err != nil {
		return nil, err
	}
	return &v1alpha2.CreateContainerResponse{
		ContainerId: name,
	}, nil
}
func (mapper *PortodshimMapper) StartContainer(ctx context.Context, req *v1alpha2.StartContainerRequest) (*v1alpha2.StartContainerResponse, error) {
	name := req.ContainerId
	err := mapper.portoClient.Start(name)
	if err != nil {
		return nil, err
	}
	return &v1alpha2.StartContainerResponse{}, nil
}
func (mapper *PortodshimMapper) StopContainer(ctx context.Context, req *v1alpha2.StopContainerRequest) (*v1alpha2.StopContainerResponse, error) {
	name := req.ContainerId
	err := mapper.portoClient.Stop(name)
	if err != nil {
		return nil, err
	}
	return &v1alpha2.StopContainerResponse{}, nil
}
func (mapper *PortodshimMapper) RemoveContainer(ctx context.Context, req *v1alpha2.RemoveContainerRequest) (*v1alpha2.RemoveContainerResponse, error) {
	name := req.ContainerId
	err := mapper.portoClient.Destroy(name)
	if err != nil {
		return nil, err
	}
	return &v1alpha2.RemoveContainerResponse{}, nil
}
func (mapper *PortodshimMapper) ListContainers(ctx context.Context, req *v1alpha2.ListContainersRequest) (*v1alpha2.ListContainersResponse, error) {
	response, err := mapper.portoClient.List()
	if err != nil {
		return nil, fmt.Errorf("error in PortodshimMapper.ListContainers(): %v", err)
	}
	var containers []*v1alpha2.Container
	for _, name := range response {
		// state
		state, err := mapper.portoClient.GetProperty(name, "state")
		if err != nil {
			return nil, err
		}

		// creation time
		creation_time_raw, err := mapper.portoClient.GetProperty(name, "creation_time[raw]")
		if err != nil {
			return nil, fmt.Errorf("error in PortodshimMapper.ListContainers(): %v", err)
		}
		creation_time, err := strconv.ParseInt(creation_time_raw, 10, 64)
		if err != nil {
			return nil, fmt.Errorf("error in PortodshimMapper.ListContainers(): %v", err)
		}

		containers = append(containers, &v1alpha2.Container{
			Id:           name,
			PodSandboxId: "None",
			Metadata: &v1alpha2.ContainerMetadata{
				Name: name,
			},
			Image: &v1alpha2.ImageSpec{
				Image: "None",
			},
			ImageRef:  "None",
			State:     mapper.stateMap[state],
			CreatedAt: time.Unix(creation_time, 0).UnixNano(),
		})
	}
	return &v1alpha2.ListContainersResponse{
		Containers: containers,
	}, nil
}
func (mapper *PortodshimMapper) ContainerStatus(ctx context.Context, req *v1alpha2.ContainerStatusRequest) (*v1alpha2.ContainerStatusResponse, error) {
	name := req.ContainerId
	response, err := mapper.portoClient.GetProperty(name, "state")
	if err != nil {
		return nil, err
	}
	return &v1alpha2.ContainerStatusResponse{
		Status: &v1alpha2.ContainerStatus{
			Id: name,
			Metadata: &v1alpha2.ContainerMetadata{
				Name: name,
			},
			State: mapper.stateMap[response],
		},
	}, nil
}
func (mapper *PortodshimMapper) UpdateContainerResources(ctx context.Context, req *v1alpha2.UpdateContainerResourcesRequest) (*v1alpha2.UpdateContainerResourcesResponse, error) {
	return nil, fmt.Errorf("not implemented UpdateContainerResources")
}
func (mapper *PortodshimMapper) ReopenContainerLog(ctx context.Context, req *v1alpha2.ReopenContainerLogRequest) (*v1alpha2.ReopenContainerLogResponse, error) {
	return nil, fmt.Errorf("not implemented ReopenContainerLog")
}
func (mapper *PortodshimMapper) ExecSync(ctx context.Context, req *v1alpha2.ExecSyncRequest) (*v1alpha2.ExecSyncResponse, error) {
	return nil, fmt.Errorf("not implemented ExecSync")
}
func (mapper *PortodshimMapper) Exec(ctx context.Context, req *v1alpha2.ExecRequest) (*v1alpha2.ExecResponse, error) {
	return nil, fmt.Errorf("not implemented Exec")
}
func (mapper *PortodshimMapper) Attach(ctx context.Context, req *v1alpha2.AttachRequest) (*v1alpha2.AttachResponse, error) {
	return nil, fmt.Errorf("not implemented Attach")
}
func (mapper *PortodshimMapper) PortForward(ctx context.Context, req *v1alpha2.PortForwardRequest) (*v1alpha2.PortForwardResponse, error) {
	return nil, fmt.Errorf("not implemented PortForward")
}
func (mapper *PortodshimMapper) ContainerStats(ctx context.Context, req *v1alpha2.ContainerStatsRequest) (*v1alpha2.ContainerStatsResponse, error) {
	return nil, fmt.Errorf("not implemented ContainerStats")
}
func (mapper *PortodshimMapper) ListContainerStats(ctx context.Context, req *v1alpha2.ListContainerStatsRequest) (*v1alpha2.ListContainerStatsResponse, error) {
	return nil, fmt.Errorf("not implemented ListContainerStats")
}
func (mapper *PortodshimMapper) ListPodSandboxStats(ctx context.Context, req *v1alpha2.ListPodSandboxStatsRequest) (*v1alpha2.ListPodSandboxStatsResponse, error) {
	return nil, fmt.Errorf("not implemented ListPodSandboxStats")
}
func (mapper *PortodshimMapper) UpdateRuntimeConfig(ctx context.Context, req *v1alpha2.UpdateRuntimeConfigRequest) (*v1alpha2.UpdateRuntimeConfigResponse, error) {
	return nil, fmt.Errorf("not implemented UpdateRuntimeConfig")
}
func (mapper *PortodshimMapper) Status(ctx context.Context, req *v1alpha2.StatusRequest) (*v1alpha2.StatusResponse, error) {
	return nil, fmt.Errorf("not implemented Status")
}
