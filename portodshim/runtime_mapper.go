package main

import (
	"context"
	"fmt"
	v1alpha2 "k8s.io/cri-api/pkg/apis/runtime/v1alpha2"
	"strconv"
	"strings"
	"time"
)

const (
	APIVersion    = "0.1.0"
	containerName = "porto"
)

type PortodshimRuntimeMapper struct {
	portoClient       API
	containerStateMap map[string]v1alpha2.ContainerState
	podStateMap       map[string]v1alpha2.PodSandboxState
}

// INTERNAL
func (mapper *PortodshimRuntimeMapper) getContainerState(id string) v1alpha2.ContainerState {
	state, err := mapper.portoClient.GetProperty(id, "state")
	if err != nil {
		return v1alpha2.ContainerState_CONTAINER_UNKNOWN
	}
	return mapper.containerStateMap[state]
}
func (mapper *PortodshimRuntimeMapper) getPodState(id string) v1alpha2.PodSandboxState {
	state, err := mapper.portoClient.GetProperty(id, "state")
	if err != nil {
		return v1alpha2.PodSandboxState_SANDBOX_NOTREADY
	}
	return mapper.podStateMap[state]
}
func (mapper *PortodshimRuntimeMapper) getCreationTime(id string) (int64, error) {
	creationTimeRaw, err := mapper.portoClient.GetProperty(id, "creation_time[raw]")
	if err != nil {
		return 0, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	creationTime, err := strconv.ParseInt(creationTimeRaw, 10, 64)
	if err != nil {
		return 0, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	return time.Unix(creationTime, 0).UnixNano(), nil
}
func (mapper *PortodshimRuntimeMapper) getPodAndContainer(id string) (string, string) {
	pod := strings.Split(id, "/")[0]
	name := ""
	if len(pod) != len(id) {
		name = id[len(pod)+1:]
	}
	return pod, name
}
func (mapper *PortodshimRuntimeMapper) getId(pod string, name string) string {
	return pod + "/" + name
}

// RUNTIME SERVICE INTERFACE
func (mapper *PortodshimRuntimeMapper) Version(ctx context.Context, req *v1alpha2.VersionRequest) (*v1alpha2.VersionResponse, error) {
	tag, rev, err := mapper.portoClient.GetVersion()
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	return &v1alpha2.VersionResponse{
		Version:           APIVersion,
		RuntimeName:       containerName,
		RuntimeVersion:    tag,
		RuntimeApiVersion: rev,
	}, nil
}
func (mapper *PortodshimRuntimeMapper) RunPodSandbox(ctx context.Context, req *v1alpha2.RunPodSandboxRequest) (*v1alpha2.RunPodSandboxResponse, error) {
	pod := req.Config.Metadata.Name
	if _, err := mapper.portoClient.GetProperty(pod, "state"); err != nil {
		err = mapper.portoClient.Create(pod)
		if err != nil {
			return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
		}
	}
	if mapper.getPodState(pod) == v1alpha2.PodSandboxState_SANDBOX_NOTREADY {
		_ = mapper.portoClient.Stop(pod)
		err := mapper.portoClient.Start(pod)
		if err != nil {
			return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
		}
	}
	return &v1alpha2.RunPodSandboxResponse{
		PodSandboxId: pod,
	}, nil
}
func (mapper *PortodshimRuntimeMapper) StopPodSandbox(ctx context.Context, req *v1alpha2.StopPodSandboxRequest) (*v1alpha2.StopPodSandboxResponse, error) {
	pod := req.PodSandboxId
	err := mapper.portoClient.Stop(pod)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	return &v1alpha2.StopPodSandboxResponse{}, nil
}
func (mapper *PortodshimRuntimeMapper) RemovePodSandbox(ctx context.Context, req *v1alpha2.RemovePodSandboxRequest) (*v1alpha2.RemovePodSandboxResponse, error) {
	pod := req.PodSandboxId
	err := mapper.portoClient.Destroy(pod)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	return &v1alpha2.RemovePodSandboxResponse{}, nil
}
func (mapper *PortodshimRuntimeMapper) PodSandboxStatus(ctx context.Context, req *v1alpha2.PodSandboxStatusRequest) (*v1alpha2.PodSandboxStatusResponse, error) {
	pod := req.PodSandboxId
	creationTime, err := mapper.getCreationTime(pod)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	return &v1alpha2.PodSandboxStatusResponse{
		Status: &v1alpha2.PodSandboxStatus{
			Id: pod,
			Metadata: &v1alpha2.PodSandboxMetadata{
				Name: pod,
			},
			State:     mapper.getPodState(pod),
			CreatedAt: creationTime,
		},
	}, nil
}
func (mapper *PortodshimRuntimeMapper) PodSandboxStats(ctx context.Context, req *v1alpha2.PodSandboxStatsRequest) (*v1alpha2.PodSandboxStatsResponse, error) {
	return nil, fmt.Errorf("not implemented PodSandboxStats")
}
func (mapper *PortodshimRuntimeMapper) ListPodSandbox(ctx context.Context, req *v1alpha2.ListPodSandboxRequest) (*v1alpha2.ListPodSandboxResponse, error) {
	response, err := mapper.portoClient.List1("*")
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	var items []*v1alpha2.PodSandbox
	for _, id := range response {
		// creation time
		creationTime, err := mapper.getCreationTime(id)
		if err != nil {
			return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
		}

		items = append(items, &v1alpha2.PodSandbox{
			Id: id,
			Metadata: &v1alpha2.PodSandboxMetadata{
				Name: id,
			},
			State:     mapper.getPodState(id),
			CreatedAt: creationTime,
		})
	}
	return &v1alpha2.ListPodSandboxResponse{
		Items: items,
	}, nil
}
func (mapper *PortodshimRuntimeMapper) CreateContainer(ctx context.Context, req *v1alpha2.CreateContainerRequest) (*v1alpha2.CreateContainerResponse, error) {
	pod := req.PodSandboxId
	name := req.Config.Metadata.Name
	id := mapper.getId(pod, name)
	err := mapper.portoClient.Create(id)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	return &v1alpha2.CreateContainerResponse{
		ContainerId: id,
	}, nil
}
func (mapper *PortodshimRuntimeMapper) StartContainer(ctx context.Context, req *v1alpha2.StartContainerRequest) (*v1alpha2.StartContainerResponse, error) {
	id := req.ContainerId
	err := mapper.portoClient.Start(id)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	return &v1alpha2.StartContainerResponse{}, nil
}
func (mapper *PortodshimRuntimeMapper) StopContainer(ctx context.Context, req *v1alpha2.StopContainerRequest) (*v1alpha2.StopContainerResponse, error) {
	id := req.ContainerId
	err := mapper.portoClient.Stop(id)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	return &v1alpha2.StopContainerResponse{}, nil
}
func (mapper *PortodshimRuntimeMapper) RemoveContainer(ctx context.Context, req *v1alpha2.RemoveContainerRequest) (*v1alpha2.RemoveContainerResponse, error) {
	id := req.ContainerId
	err := mapper.portoClient.Destroy(id)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	return &v1alpha2.RemoveContainerResponse{}, nil
}
func (mapper *PortodshimRuntimeMapper) ListContainers(ctx context.Context, req *v1alpha2.ListContainersRequest) (*v1alpha2.ListContainersResponse, error) {
	response, err := mapper.portoClient.List()
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	var containers []*v1alpha2.Container
	for _, id := range response {
		// pod and name
		pod, name := mapper.getPodAndContainer(id)
		if name == "" {
			// skip containers with level = 1
			continue
		}

		// creation time
		creationTime, err := mapper.getCreationTime(id)
		if err != nil {
			return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
		}

		containers = append(containers, &v1alpha2.Container{
			Id:           id,
			PodSandboxId: pod,
			Metadata: &v1alpha2.ContainerMetadata{
				Name: name,
			},
			Image: &v1alpha2.ImageSpec{
				Image: "None",
			},
			ImageRef:  "None",
			State:     mapper.getContainerState(id),
			CreatedAt: creationTime,
		})
	}
	return &v1alpha2.ListContainersResponse{
		Containers: containers,
	}, nil
}
func (mapper *PortodshimRuntimeMapper) ContainerStatus(ctx context.Context, req *v1alpha2.ContainerStatusRequest) (*v1alpha2.ContainerStatusResponse, error) {
	id := req.ContainerId
	return &v1alpha2.ContainerStatusResponse{
		Status: &v1alpha2.ContainerStatus{
			Id: id,
			Metadata: &v1alpha2.ContainerMetadata{
				Name: id,
			},
			State: mapper.getContainerState(id),
		},
	}, nil
}
func (mapper *PortodshimRuntimeMapper) UpdateContainerResources(ctx context.Context, req *v1alpha2.UpdateContainerResourcesRequest) (*v1alpha2.UpdateContainerResourcesResponse, error) {
	return nil, fmt.Errorf("not implemented UpdateContainerResources")
}
func (mapper *PortodshimRuntimeMapper) ReopenContainerLog(ctx context.Context, req *v1alpha2.ReopenContainerLogRequest) (*v1alpha2.ReopenContainerLogResponse, error) {
	return nil, fmt.Errorf("not implemented ReopenContainerLog")
}
func (mapper *PortodshimRuntimeMapper) ExecSync(ctx context.Context, req *v1alpha2.ExecSyncRequest) (*v1alpha2.ExecSyncResponse, error) {
	return nil, fmt.Errorf("not implemented ExecSync")
}
func (mapper *PortodshimRuntimeMapper) Exec(ctx context.Context, req *v1alpha2.ExecRequest) (*v1alpha2.ExecResponse, error) {
	return nil, fmt.Errorf("not implemented Exec")
}
func (mapper *PortodshimRuntimeMapper) Attach(ctx context.Context, req *v1alpha2.AttachRequest) (*v1alpha2.AttachResponse, error) {
	return nil, fmt.Errorf("not implemented Attach")
}
func (mapper *PortodshimRuntimeMapper) PortForward(ctx context.Context, req *v1alpha2.PortForwardRequest) (*v1alpha2.PortForwardResponse, error) {
	return nil, fmt.Errorf("not implemented PortForward")
}
func (mapper *PortodshimRuntimeMapper) ContainerStats(ctx context.Context, req *v1alpha2.ContainerStatsRequest) (*v1alpha2.ContainerStatsResponse, error) {
	return nil, fmt.Errorf("not implemented ContainerStats")
}
func (mapper *PortodshimRuntimeMapper) ListContainerStats(ctx context.Context, req *v1alpha2.ListContainerStatsRequest) (*v1alpha2.ListContainerStatsResponse, error) {
	return nil, fmt.Errorf("not implemented ListContainerStats")
}
func (mapper *PortodshimRuntimeMapper) ListPodSandboxStats(ctx context.Context, req *v1alpha2.ListPodSandboxStatsRequest) (*v1alpha2.ListPodSandboxStatsResponse, error) {
	return nil, fmt.Errorf("not implemented ListPodSandboxStats")
}
func (mapper *PortodshimRuntimeMapper) UpdateRuntimeConfig(ctx context.Context, req *v1alpha2.UpdateRuntimeConfigRequest) (*v1alpha2.UpdateRuntimeConfigResponse, error) {
	return nil, fmt.Errorf("not implemented UpdateRuntimeConfig")
}
func (mapper *PortodshimRuntimeMapper) Status(ctx context.Context, req *v1alpha2.StatusRequest) (*v1alpha2.StatusResponse, error) {
	return nil, fmt.Errorf("not implemented Status")
}
