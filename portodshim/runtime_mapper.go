package main

import (
	"context"
	"fmt"
	"go.uber.org/zap"
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
func (mapper *PortodshimRuntimeMapper) getTimeProperty(id string, property string) int64 {
	return time.Unix(int64(mapper.getUintProperty(id, property)), 0).UnixNano()
}
func (mapper *PortodshimRuntimeMapper) getUintProperty(id string, property string) uint64 {
	valueString, err := mapper.portoClient.GetProperty(id, property)
	if err != nil || valueString == "" {
		return 0
	}

	value, err := strconv.ParseUint(valueString, 10, 64)
	if err != nil {
		zap.S().Warnf("%s: %v", getCurrentFuncName(), err)
		return 0
	}

	return value
}
func (mapper *PortodshimRuntimeMapper) getStringProperty(id string, property string) string {
	value, err := mapper.portoClient.GetProperty(id, property)
	if err != nil {
		return ""
	}

	return value
}
func (mapper *PortodshimRuntimeMapper) getContainerStats(id string) (*v1alpha2.ContainerStats, error) {
	_, name := mapper.getPodAndContainer(id)
	if name == "" {
		return nil, fmt.Errorf("%s: cannot get container stats: specified ID belongs to pod", getCurrentFuncName())
	}

	cpu := mapper.getUintProperty(id, "cpu_usage")
	timestamp := time.Now().UnixNano()

	// TODO: Заполнить оставшиеся метрики
	return &v1alpha2.ContainerStats{
		Attributes: &v1alpha2.ContainerAttributes{
			Id: id,
			Metadata: &v1alpha2.ContainerMetadata{
				Name: name,
			},
		},
		Cpu: &v1alpha2.CpuUsage{
			Timestamp:            timestamp,
			UsageCoreNanoSeconds: &v1alpha2.UInt64Value{Value: cpu},
			UsageNanoCores:       &v1alpha2.UInt64Value{Value: cpu / 1000000000},
		},
		Memory: &v1alpha2.MemoryUsage{
			Timestamp:       timestamp,
			WorkingSetBytes: &v1alpha2.UInt64Value{Value: 0},
			AvailableBytes:  &v1alpha2.UInt64Value{Value: 0},
			UsageBytes:      &v1alpha2.UInt64Value{Value: mapper.getUintProperty(id, "memory_usage")},
			RssBytes:        &v1alpha2.UInt64Value{Value: 0},
			PageFaults:      &v1alpha2.UInt64Value{Value: mapper.getUintProperty(id, "minor_faults")},
			MajorPageFaults: &v1alpha2.UInt64Value{Value: mapper.getUintProperty(id, "major_faults")},
		},
		WritableLayer: &v1alpha2.FilesystemUsage{
			Timestamp: timestamp,
			FsId: &v1alpha2.FilesystemIdentifier{
				Mountpoint: "/",
			},
			UsedBytes:  &v1alpha2.UInt64Value{Value: 0},
			InodesUsed: &v1alpha2.UInt64Value{Value: 0},
		},
	}, nil
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
	id := req.GetConfig().GetMetadata().GetName()

	if _, err := mapper.portoClient.GetProperty(id, "state"); err != nil {
		err = mapper.portoClient.Create(id)
		if err != nil {
			return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
		}
	}

	if mapper.getPodState(id) == v1alpha2.PodSandboxState_SANDBOX_NOTREADY {
		_ = mapper.portoClient.Stop(id)
		err := mapper.portoClient.Start(id)
		if err != nil {
			return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
		}
	}

	return &v1alpha2.RunPodSandboxResponse{
		PodSandboxId: id,
	}, nil
}
func (mapper *PortodshimRuntimeMapper) StopPodSandbox(ctx context.Context, req *v1alpha2.StopPodSandboxRequest) (*v1alpha2.StopPodSandboxResponse, error) {
	id := req.GetPodSandboxId()

	err := mapper.portoClient.Stop(id)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	return &v1alpha2.StopPodSandboxResponse{}, nil
}
func (mapper *PortodshimRuntimeMapper) RemovePodSandbox(ctx context.Context, req *v1alpha2.RemovePodSandboxRequest) (*v1alpha2.RemovePodSandboxResponse, error) {
	id := req.GetPodSandboxId()

	err := mapper.portoClient.Destroy(id)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	return &v1alpha2.RemovePodSandboxResponse{}, nil
}
func (mapper *PortodshimRuntimeMapper) PodSandboxStatus(ctx context.Context, req *v1alpha2.PodSandboxStatusRequest) (*v1alpha2.PodSandboxStatusResponse, error) {
	id := req.GetPodSandboxId()

	return &v1alpha2.PodSandboxStatusResponse{
		Status: &v1alpha2.PodSandboxStatus{
			Id: id,
			Metadata: &v1alpha2.PodSandboxMetadata{
				Name:      id,
				Uid:       id,
				Namespace: "default",
			},
			State:     mapper.getPodState(id),
			CreatedAt: mapper.getTimeProperty(id, "creation_time[raw]"),
			Network: &v1alpha2.PodSandboxNetworkStatus{
				Ip: "",
			},
			Linux: &v1alpha2.LinuxPodSandboxStatus{
				Namespaces: &v1alpha2.Namespace{
					Options: &v1alpha2.NamespaceOption{
						Network: v1alpha2.NamespaceMode_NODE,
						Pid:     v1alpha2.NamespaceMode_CONTAINER,
						Ipc:     v1alpha2.NamespaceMode_CONTAINER,
					},
				},
			},
		},
	}, nil
}
func (mapper *PortodshimRuntimeMapper) PodSandboxStats(ctx context.Context, req *v1alpha2.PodSandboxStatsRequest) (*v1alpha2.PodSandboxStatsResponse, error) {
	id := req.GetPodSandboxId()
	_, name := mapper.getPodAndContainer(id)
	if name != "" {
		return nil, fmt.Errorf("%s: specified ID belongs to container", getCurrentFuncName())
	}

	cpu := mapper.getUintProperty(id, "cpu_usage")

	response, err := mapper.portoClient.List1(id + "/***")
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	var stats []*v1alpha2.ContainerStats
	for _, ctrId := range response {
		st, err := mapper.getContainerStats(ctrId)
		if err != nil {
			continue
		}
		stats = append(stats, st)
	}

	timestamp := time.Now().UnixNano()

	// TODO: Заполнить оставшиеся метрики
	return &v1alpha2.PodSandboxStatsResponse{
		Stats: &v1alpha2.PodSandboxStats{
			Attributes: &v1alpha2.PodSandboxAttributes{
				Id: id,
				Metadata: &v1alpha2.PodSandboxMetadata{
					Name: name,
				},
			},
			Linux: &v1alpha2.LinuxPodSandboxStats{
				Cpu: &v1alpha2.CpuUsage{
					Timestamp:            timestamp,
					UsageCoreNanoSeconds: &v1alpha2.UInt64Value{Value: cpu},
					UsageNanoCores:       &v1alpha2.UInt64Value{Value: cpu / 1000000000},
				},
				Memory: &v1alpha2.MemoryUsage{
					Timestamp:       timestamp,
					WorkingSetBytes: &v1alpha2.UInt64Value{Value: 0},
					AvailableBytes:  &v1alpha2.UInt64Value{Value: 0},
					UsageBytes:      &v1alpha2.UInt64Value{Value: mapper.getUintProperty(id, "memory_usage")},
					RssBytes:        &v1alpha2.UInt64Value{Value: 0},
					PageFaults:      &v1alpha2.UInt64Value{Value: mapper.getUintProperty(id, "minor_faults")},
					MajorPageFaults: &v1alpha2.UInt64Value{Value: mapper.getUintProperty(id, "major_faults")},
				},
				Network: &v1alpha2.NetworkUsage{
					Timestamp: timestamp,
					DefaultInterface: &v1alpha2.NetworkInterfaceUsage{
						Name:     "eth0",
						RxBytes:  &v1alpha2.UInt64Value{Value: mapper.getUintProperty(id, "net_rx_bytes")},
						RxErrors: &v1alpha2.UInt64Value{Value: 0},
						TxBytes:  &v1alpha2.UInt64Value{Value: mapper.getUintProperty(id, "net_bytes")},
						TxErrors: &v1alpha2.UInt64Value{Value: 0},
					},
				},
				Process: &v1alpha2.ProcessUsage{
					Timestamp:    timestamp,
					ProcessCount: &v1alpha2.UInt64Value{Value: mapper.getUintProperty(id, "process_count")},
				},
				Containers: stats,
			},
			Windows: &v1alpha2.WindowsPodSandboxStats{},
		},
	}, nil
}
func (mapper *PortodshimRuntimeMapper) ListPodSandbox(ctx context.Context, req *v1alpha2.ListPodSandboxRequest) (*v1alpha2.ListPodSandboxResponse, error) {
	// TODO: Добавить фильтры
	response, err := mapper.portoClient.List1("*")
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	var items []*v1alpha2.PodSandbox
	for _, id := range response {
		items = append(items, &v1alpha2.PodSandbox{
			Id: id,
			Metadata: &v1alpha2.PodSandboxMetadata{
				Name: id,
			},
			State:     mapper.getPodState(id),
			CreatedAt: mapper.getTimeProperty(id, "creation_time[raw]"),
		})
	}

	return &v1alpha2.ListPodSandboxResponse{
		Items: items,
	}, nil
}
func (mapper *PortodshimRuntimeMapper) CreateContainer(ctx context.Context, req *v1alpha2.CreateContainerRequest) (*v1alpha2.CreateContainerResponse, error) {
	pod := req.GetPodSandboxId()
	name := req.GetConfig().GetMetadata().GetName()
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
	id := req.GetContainerId()

	err := mapper.portoClient.Start(id)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	return &v1alpha2.StartContainerResponse{}, nil
}
func (mapper *PortodshimRuntimeMapper) StopContainer(ctx context.Context, req *v1alpha2.StopContainerRequest) (*v1alpha2.StopContainerResponse, error) {
	id := req.GetContainerId()

	err := mapper.portoClient.Stop(id)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	return &v1alpha2.StopContainerResponse{}, nil
}
func (mapper *PortodshimRuntimeMapper) RemoveContainer(ctx context.Context, req *v1alpha2.RemoveContainerRequest) (*v1alpha2.RemoveContainerResponse, error) {
	id := req.GetContainerId()

	err := mapper.portoClient.Destroy(id)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	return &v1alpha2.RemoveContainerResponse{}, nil
}
func (mapper *PortodshimRuntimeMapper) ListContainers(ctx context.Context, req *v1alpha2.ListContainersRequest) (*v1alpha2.ListContainersResponse, error) {
	// TODO: Добавить фильтры
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
			CreatedAt: mapper.getTimeProperty(id, "creation_time[raw]"),
		})
	}

	return &v1alpha2.ListContainersResponse{
		Containers: containers,
	}, nil
}
func (mapper *PortodshimRuntimeMapper) ContainerStatus(ctx context.Context, req *v1alpha2.ContainerStatusRequest) (*v1alpha2.ContainerStatusResponse, error) {
	id := req.GetContainerId()

	return &v1alpha2.ContainerStatusResponse{
		Status: &v1alpha2.ContainerStatus{
			Id: id,
			Metadata: &v1alpha2.ContainerMetadata{
				Name: id,
			},
			State:      mapper.getContainerState(id),
			CreatedAt:  mapper.getTimeProperty(id, "creation_time[raw]"),
			StartedAt:  mapper.getTimeProperty(id, "start_time[raw]"),
			FinishedAt: mapper.getTimeProperty(id, "death_time[raw]"),
			ExitCode:   int32(mapper.getUintProperty(id, "exit_code")),
			Image: &v1alpha2.ImageSpec{
				Image: "",
			},
			ImageRef: "",
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
	st, err := mapper.getContainerStats(req.GetContainerId())
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	return &v1alpha2.ContainerStatsResponse{
		Stats: st,
	}, nil
}
func (mapper *PortodshimRuntimeMapper) ListContainerStats(ctx context.Context, req *v1alpha2.ListContainerStatsRequest) (*v1alpha2.ListContainerStatsResponse, error) {
	// TODO: Добавить фильтр по labels
	filterId := req.GetFilter().GetId()
	if filterId != "" {
		st, err := mapper.getContainerStats(filterId)
		if err != nil {
			return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
		}

		return &v1alpha2.ListContainerStatsResponse{
			Stats: []*v1alpha2.ContainerStats{st},
		}, nil
	}

	filterPod := req.GetFilter().GetPodSandboxId()
	mask := ""
	if filterPod != "" {
		mask = filterPod + "/***"
	}

	response, err := mapper.portoClient.List1(mask)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	var stats []*v1alpha2.ContainerStats
	for _, id := range response {
		st, err := mapper.getContainerStats(id)
		if err != nil {
			continue
		}
		stats = append(stats, st)
	}

	return &v1alpha2.ListContainerStatsResponse{
		Stats: stats,
	}, nil
}
func (mapper *PortodshimRuntimeMapper) ListPodSandboxStats(ctx context.Context, req *v1alpha2.ListPodSandboxStatsRequest) (*v1alpha2.ListPodSandboxStatsResponse, error) {
	return nil, fmt.Errorf("not implemented ListPodSandboxStats")
}
func (mapper *PortodshimRuntimeMapper) UpdateRuntimeConfig(ctx context.Context, req *v1alpha2.UpdateRuntimeConfigRequest) (*v1alpha2.UpdateRuntimeConfigResponse, error) {
	return nil, fmt.Errorf("not implemented UpdateRuntimeConfig")
}
func (mapper *PortodshimRuntimeMapper) Status(ctx context.Context, req *v1alpha2.StatusRequest) (*v1alpha2.StatusResponse, error) {
	if _, _, err := mapper.portoClient.GetVersion(); err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	var conditions []*v1alpha2.RuntimeCondition
	conditions = append(conditions, &v1alpha2.RuntimeCondition{
		Type:   "RuntimeReady",
		Status: true,
	})
	conditions = append(conditions, &v1alpha2.RuntimeCondition{
		Type:   "NetworkReady",
		Status: true,
	})
	return &v1alpha2.StatusResponse{
		Status: &v1alpha2.RuntimeStatus{
			Conditions: conditions,
		},
	}, nil
}
