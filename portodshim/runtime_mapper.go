package main

import (
	"context"
	"encoding/base64"
	"fmt"
	"math/rand"
	"os"
	"strconv"
	"strings"
	"time"

	"go.uber.org/zap"
	v1 "k8s.io/cri-api/pkg/apis/runtime/v1"
)

const (
	containerName = "porto"
	VolumesPath   = "/place/portodshim_volumes"
)

type PortodshimRuntimeMapper struct {
	containerStateMap map[string]v1.ContainerState
	podStateMap       map[string]v1.PodSandboxState
	randGenerator     *rand.Rand
}

func NewPortodshimRuntimeMapper() PortodshimRuntimeMapper {
	runtimeMapper := PortodshimRuntimeMapper{}

	runtimeMapper.containerStateMap = map[string]v1.ContainerState{
		"stopped":    v1.ContainerState_CONTAINER_CREATED,
		"paused":     v1.ContainerState_CONTAINER_RUNNING,
		"starting":   v1.ContainerState_CONTAINER_RUNNING,
		"running":    v1.ContainerState_CONTAINER_RUNNING,
		"stopping":   v1.ContainerState_CONTAINER_RUNNING,
		"respawning": v1.ContainerState_CONTAINER_RUNNING,
		"meta":       v1.ContainerState_CONTAINER_RUNNING,
		"dead":       v1.ContainerState_CONTAINER_EXITED,
	}

	runtimeMapper.podStateMap = map[string]v1.PodSandboxState{
		"stopped":    v1.PodSandboxState_SANDBOX_NOTREADY,
		"paused":     v1.PodSandboxState_SANDBOX_NOTREADY,
		"starting":   v1.PodSandboxState_SANDBOX_NOTREADY,
		"running":    v1.PodSandboxState_SANDBOX_READY,
		"stopping":   v1.PodSandboxState_SANDBOX_NOTREADY,
		"respawning": v1.PodSandboxState_SANDBOX_NOTREADY,
		"meta":       v1.PodSandboxState_SANDBOX_NOTREADY,
		"dead":       v1.PodSandboxState_SANDBOX_NOTREADY,
	}

	runtimeMapper.randGenerator = rand.New(rand.NewSource(time.Now().UnixNano()))

	return runtimeMapper
}

// INTERNAL
func (mapper *PortodshimRuntimeMapper) getContainerState(ctx context.Context, id string) v1.ContainerState {
	portoClient := ctx.Value("portoClient").(API)

	state, err := portoClient.GetProperty(id, "state")
	if err != nil {
		return v1.ContainerState_CONTAINER_UNKNOWN
	}

	return mapper.containerStateMap[state]
}
func (mapper *PortodshimRuntimeMapper) getPodState(ctx context.Context, id string) v1.PodSandboxState {
	portoClient := ctx.Value("portoClient").(API)

	state, err := portoClient.GetProperty(id, "state")
	if err != nil {
		return v1.PodSandboxState_SANDBOX_NOTREADY
	}

	return mapper.podStateMap[state]
}
func (mapper *PortodshimRuntimeMapper) getPodAndContainer(id string) (string, string) {
	// <id> := <podId>/<containerId>
	podId := strings.Split(id, "/")[0]
	containerId := ""
	if len(podId) != len(id) {
		containerId = id[len(podId)+1:]
	}

	return podId, containerId
}
func (mapper *PortodshimRuntimeMapper) isContainer(id string) bool {
	_, containerId := mapper.getPodAndContainer(id)
	return containerId != ""
}
func (mapper *PortodshimRuntimeMapper) createId(name string) string {
	length := 26
	if len(name) < length {
		length = len(name)
	}
	// max length of return value is 26 + 1 + 4 = 31, so container id <= 63
	return fmt.Sprintf("%s-%x", name[:length], mapper.randGenerator.Uint32()%65536)
}
func (mapper *PortodshimRuntimeMapper) convertBase64(src string, encode bool) string {
	if encode {
		return base64.RawStdEncoding.EncodeToString([]byte(src))
	}

	dst, err := base64.RawStdEncoding.DecodeString(src)
	if err != nil {
		return src
	}

	return string(dst)
}
func (mapper *PortodshimRuntimeMapper) convertLabel(src string, toPorto bool, prefix string) string {
	dst := src
	if toPorto {
		dst = mapper.convertBase64(dst, true)
		if prefix != "" {
			dst = prefix + "." + dst
		}
	} else {
		if prefix != "" {
			dst = strings.TrimPrefix(dst, prefix+".")
		}
		dst = mapper.convertBase64(dst, false)
	}

	return dst
}
func (mapper *PortodshimRuntimeMapper) setLabels(ctx context.Context, id string, labels map[string]string, prefix string) {
	portoClient := ctx.Value("portoClient").(API)

	labelsString := ""
	for label, value := range labels {
		labelsString += fmt.Sprintf("%s:%s;", mapper.convertLabel(label, true, prefix), mapper.convertLabel(value, true, ""))
	}

	err := portoClient.SetProperty(id, "labels", labelsString)
	if err != nil {
		zap.S().Warnf("%s: cannot set labels %v", getCurrentFuncName(), err)
	}
}
func (mapper *PortodshimRuntimeMapper) getLabels(ctx context.Context, id string, prefix string) map[string]string {
	portoClient := ctx.Value("portoClient").(API)

	labels, err := portoClient.GetProperty(id, "labels")
	if err != nil {
		zap.S().Warnf("%s: %v", getCurrentFuncName(), err)
		return map[string]string{}
	}

	result := make(map[string]string)
	if len(labels) > 0 {
		// porto container labels parsing
		for _, pair := range strings.Split(labels, ";") {
			splitedPair := strings.Split(pair, ":")
			label := strings.TrimSpace(splitedPair[0])
			value := strings.TrimSpace(splitedPair[1])
			if !strings.HasPrefix(label, prefix) {
				continue
			}
			result[mapper.convertLabel(label, false, prefix)] = mapper.convertLabel(value, false, "")
		}
	}

	return result
}
func (mapper *PortodshimRuntimeMapper) getValueForKubeLabel(ctx context.Context, id string, label string, prefix string) string {
	return mapper.convertLabel(mapper.getStringProperty(ctx, id, fmt.Sprintf("labels[%s]", mapper.convertLabel(label, true, prefix))), false, "")
}
func (mapper *PortodshimRuntimeMapper) getTimeProperty(ctx context.Context, id string, property string) int64 {
	return time.Unix(int64(mapper.getUintProperty(ctx, id, property)), 0).UnixNano()
}
func (mapper *PortodshimRuntimeMapper) getUintProperty(ctx context.Context, id string, property string) uint64 {
	portoClient := ctx.Value("portoClient").(API)

	valueString, err := portoClient.GetProperty(id, property)
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
func (mapper *PortodshimRuntimeMapper) getIntProperty(ctx context.Context, id string, property string) int64 {
	portoClient := ctx.Value("portoClient").(API)

	valueString, err := portoClient.GetProperty(id, property)
	if err != nil || valueString == "" {
		return 0
	}

	value, err := strconv.ParseInt(valueString, 10, 64)
	if err != nil {
		zap.S().Warnf("%s: %v", getCurrentFuncName(), err)
		return 0
	}

	return value
}
func (mapper *PortodshimRuntimeMapper) getStringProperty(ctx context.Context, id string, property string) string {
	portoClient := ctx.Value("portoClient").(API)

	value, err := portoClient.GetProperty(id, property)
	if err != nil {
		return ""
	}

	return value
}
func (mapper *PortodshimRuntimeMapper) getPodMetadata(ctx context.Context, id string) *v1.PodSandboxMetadata {
	labels := mapper.getLabels(ctx, id, "LABEL")
	attempt, _ := strconv.ParseUint(labels["attempt"], 10, 64)

	return &v1.PodSandboxMetadata{
		Name:      labels["io.kubernetes.pod.name"],
		Uid:       labels["io.kubernetes.pod.uid"],
		Namespace: labels["io.kubernetes.pod.namespace"],
		Attempt:   uint32(attempt),
	}
}
func (mapper *PortodshimRuntimeMapper) getPodStats(ctx context.Context, id string) *v1.PodSandboxStats {
	portoClient := ctx.Value("portoClient").(API)

	cpu := mapper.getUintProperty(ctx, id, "cpu_usage")
	timestamp := time.Now().UnixNano()

	response, err := portoClient.List1(id + "/***")
	if err != nil {
		zap.S().Warnf("%s: %v", getCurrentFuncName(), err)
		return nil
	}

	var stats []*v1.ContainerStats
	for _, ctrId := range response {
		stats = append(stats, mapper.getContainerStats(ctx, ctrId))
	}

	// TODO: Заполнить оставшиеся метрики
	return &v1.PodSandboxStats{
		Attributes: &v1.PodSandboxAttributes{
			Id:          id,
			Metadata:    mapper.getPodMetadata(ctx, id),
			Labels:      mapper.getLabels(ctx, id, "LABEL"),
			Annotations: mapper.getLabels(ctx, id, "ANNOTATION"),
		},
		Linux: &v1.LinuxPodSandboxStats{
			Cpu: &v1.CpuUsage{
				Timestamp:            timestamp,
				UsageCoreNanoSeconds: &v1.UInt64Value{Value: cpu},
				UsageNanoCores:       &v1.UInt64Value{Value: cpu / 1000000000},
			},
			Memory: &v1.MemoryUsage{
				Timestamp:       timestamp,
				WorkingSetBytes: &v1.UInt64Value{Value: 0},
				AvailableBytes:  &v1.UInt64Value{Value: 0},
				UsageBytes:      &v1.UInt64Value{Value: mapper.getUintProperty(ctx, id, "memory_usage")},
				RssBytes:        &v1.UInt64Value{Value: 0},
				PageFaults:      &v1.UInt64Value{Value: mapper.getUintProperty(ctx, id, "minor_faults")},
				MajorPageFaults: &v1.UInt64Value{Value: mapper.getUintProperty(ctx, id, "major_faults")},
			},
			Network: &v1.NetworkUsage{
				Timestamp: timestamp,
				DefaultInterface: &v1.NetworkInterfaceUsage{
					Name:     "eth0",
					RxBytes:  &v1.UInt64Value{Value: mapper.getUintProperty(ctx, id, "net_rx_bytes")},
					RxErrors: &v1.UInt64Value{Value: 0},
					TxBytes:  &v1.UInt64Value{Value: mapper.getUintProperty(ctx, id, "net_bytes")},
					TxErrors: &v1.UInt64Value{Value: 0},
				},
			},
			Process: &v1.ProcessUsage{
				Timestamp:    timestamp,
				ProcessCount: &v1.UInt64Value{Value: mapper.getUintProperty(ctx, id, "process_count")},
			},
			Containers: stats,
		},
		Windows: &v1.WindowsPodSandboxStats{},
	}
}
func (mapper *PortodshimRuntimeMapper) getContainerMetadata(ctx context.Context, id string) *v1.ContainerMetadata {
	labels := mapper.getLabels(ctx, id, "LABEL")
	attempt, _ := strconv.ParseUint(labels["attempt"], 10, 64)

	return &v1.ContainerMetadata{
		Name:    labels["io.kubernetes.container.name"],
		Attempt: uint32(attempt),
	}
}
func (mapper *PortodshimRuntimeMapper) getContainerStats(ctx context.Context, id string) *v1.ContainerStats {
	cpu := mapper.getUintProperty(ctx, id, "cpu_usage")
	timestamp := time.Now().UnixNano()

	// TODO: Заполнить оставшиеся метрики
	return &v1.ContainerStats{
		Attributes: &v1.ContainerAttributes{
			Id:          id,
			Metadata:    mapper.getContainerMetadata(ctx, id),
			Labels:      mapper.getLabels(ctx, id, "LABEL"),
			Annotations: mapper.getLabels(ctx, id, "ANNOTATION"),
		},
		Cpu: &v1.CpuUsage{
			Timestamp:            timestamp,
			UsageCoreNanoSeconds: &v1.UInt64Value{Value: cpu},
			UsageNanoCores:       &v1.UInt64Value{Value: cpu / 1000000000},
		},
		Memory: &v1.MemoryUsage{
			Timestamp:       timestamp,
			WorkingSetBytes: &v1.UInt64Value{Value: 0},
			AvailableBytes:  &v1.UInt64Value{Value: 0},
			UsageBytes:      &v1.UInt64Value{Value: mapper.getUintProperty(ctx, id, "memory_usage")},
			RssBytes:        &v1.UInt64Value{Value: 0},
			PageFaults:      &v1.UInt64Value{Value: mapper.getUintProperty(ctx, id, "minor_faults")},
			MajorPageFaults: &v1.UInt64Value{Value: mapper.getUintProperty(ctx, id, "major_faults")},
		},
		WritableLayer: &v1.FilesystemUsage{
			Timestamp: timestamp,
			FsId: &v1.FilesystemIdentifier{
				Mountpoint: VolumesPath + "/" + id,
			},
			UsedBytes:  &v1.UInt64Value{Value: 0},
			InodesUsed: &v1.UInt64Value{Value: 0},
		},
	}
}
func (mapper *PortodshimRuntimeMapper) getContainerImage(ctx context.Context, id string) string {
	portoClient := ctx.Value("portoClient").(API)

	if !mapper.isContainer(id) {
		return ""
	}

	imageDescriptions, err := portoClient.ListVolumes(VolumesPath+"/"+id, id)
	if err != nil {
		zap.S().Warnf("%s: %v", getCurrentFuncName(), err)
		return ""
	}
	return imageDescriptions[0].Properties["layers"]
}
func (mapper *PortodshimRuntimeMapper) setNetwork(ctx context.Context, id string, mappings []*v1.PortMapping) {
	portoClient := ctx.Value("portoClient").(API)

	addresses := ""
	for _, mapping := range mappings {
		if ip := mapping.GetHostIp(); ip != "" {
			addresses += fmt.Sprintf("eth0 %s;", ip)
		}
	}

	// TODO: убрать заглушку
	addresses += "eth0 192.168.1.1;"

	err := portoClient.SetProperty(id, "ip", addresses)
	if err != nil {
		zap.S().Warnf("%s: %v", getCurrentFuncName(), err)
	}
}
func (mapper *PortodshimRuntimeMapper) getNetwork(ctx context.Context, id string) *v1.PodSandboxNetworkStatus {
	portoClient := ctx.Value("portoClient").(API)

	addresses, err := portoClient.GetProperty(id, "ip")
	if err != nil {
		zap.S().Warnf("%s: %v", getCurrentFuncName(), err)
		return &v1.PodSandboxNetworkStatus{}
	}

	ips := []*v1.PodIP{}
	if len(addresses) > 0 {
		for _, address := range strings.Split(addresses, ";") {
			if pair := strings.Split(address, " "); len(pair) > 1 {
				if ip := pair[1]; ip != "auto" {
					ips = append(ips, &v1.PodIP{Ip: ip})
				}
			}
		}
	}

	var status v1.PodSandboxNetworkStatus
	if len(ips) > 0 {
		status.Ip = ips[0].GetIp()
	}
	if len(ips) > 1 {
		status.AdditionalIps = ips[1:]
	}

	return &status
}

// RUNTIME SERVICE INTERFACE
func (mapper *PortodshimRuntimeMapper) Version(ctx context.Context, req *v1.VersionRequest) (*v1.VersionResponse, error) {
	zap.S().Debugf("call %s", getCurrentFuncName())

	portoClient := ctx.Value("portoClient").(API)

	tag, _, err := portoClient.GetVersion()
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	// TODO: temprorary use tag as a RuntimeApiVersion
	return &v1.VersionResponse{
		Version:           req.GetVersion(),
		RuntimeName:       containerName,
		RuntimeVersion:    tag,
		RuntimeApiVersion: tag,
	}, nil
}
func (mapper *PortodshimRuntimeMapper) RunPodSandbox(ctx context.Context, req *v1.RunPodSandboxRequest) (*v1.RunPodSandboxResponse, error) {
	zap.S().Debugf("call %s: %s", getCurrentFuncName(), req.GetConfig().GetMetadata().GetName())

	portoClient := ctx.Value("portoClient").(API)

	id := mapper.createId(req.GetConfig().GetMetadata().GetName())

	err := portoClient.Create(id)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// volume creating
	config := map[string]string{
		"backend":    "overlay",
		"containers": id,
		"layers":     "ubuntu:xenial",
	}

	rootPath := VolumesPath + "/" + id
	err = os.Mkdir(rootPath, 0755)
	if err != nil {
		if os.IsExist(err) {
			zap.S().Warnf("%s: directory already exists: %s", getCurrentFuncName(), rootPath)
		} else {
			_ = portoClient.Destroy(id)
			return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
		}
	}

	_, err = portoClient.CreateVolume(rootPath, config)
	if err != nil {
		_ = portoClient.Destroy(id)
		_ = os.RemoveAll(rootPath)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	err = portoClient.SetProperty(id, "root", rootPath)
	if err != nil {
		_ = portoClient.Destroy(id)
		_ = os.RemoveAll(rootPath)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// network
	err = portoClient.SetProperty(id, "hostname", req.GetConfig().GetHostname())
	if err != nil {
		zap.S().Warnf("%s: %v", getCurrentFuncName(), err)
	}

	mapper.setNetwork(ctx, id, req.GetConfig().GetPortMappings())

	// labels and annotations
	labels := req.GetConfig().GetLabels()
	if labels == nil {
		labels = make(map[string]string)
	}
	if _, found := labels["io.kubernetes.pod.namespace"]; !found {
		labels["io.kubernetes.pod.namespace"] = req.GetConfig().GetMetadata().GetNamespace()
	}
	labels["attempt"] = fmt.Sprint(req.GetConfig().GetMetadata().GetAttempt())

	mapper.setLabels(ctx, id, labels, "LABEL")
	mapper.setLabels(ctx, id, req.GetConfig().GetAnnotations(), "ANNOTATION")

	// command
	err = portoClient.SetProperty(id, "command", "sleep inf")
	if err != nil {
		zap.S().Warnf("%s: %v", getCurrentFuncName(), err)
	}

	// pod starting
	err = portoClient.Start(id)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	return &v1.RunPodSandboxResponse{
		PodSandboxId: id,
	}, nil
}
func (mapper *PortodshimRuntimeMapper) StopPodSandbox(ctx context.Context, req *v1.StopPodSandboxRequest) (*v1.StopPodSandboxResponse, error) {
	zap.S().Debugf("call %s: %s", getCurrentFuncName(), req.GetPodSandboxId())

	portoClient := ctx.Value("portoClient").(API)

	id := req.GetPodSandboxId()

	if state := mapper.getStringProperty(ctx, id, "state"); state == "running" {
		err := portoClient.Kill(id, 15)
		if err != nil {
			return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
		}
	}

	return &v1.StopPodSandboxResponse{}, nil
}
func (mapper *PortodshimRuntimeMapper) RemovePodSandbox(ctx context.Context, req *v1.RemovePodSandboxRequest) (*v1.RemovePodSandboxResponse, error) {
	zap.S().Debugf("call %s: %s", getCurrentFuncName(), req.GetPodSandboxId())

	portoClient := ctx.Value("portoClient").(API)

	id := req.GetPodSandboxId()

	err := portoClient.Destroy(id)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	rootPath := VolumesPath + "/" + id
	err = os.RemoveAll(rootPath)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	return &v1.RemovePodSandboxResponse{}, nil
}
func (mapper *PortodshimRuntimeMapper) PodSandboxStatus(ctx context.Context, req *v1.PodSandboxStatusRequest) (*v1.PodSandboxStatusResponse, error) {
	zap.S().Debugf("call %s: %s", getCurrentFuncName(), req.GetPodSandboxId())

	id := req.GetPodSandboxId()

	return &v1.PodSandboxStatusResponse{
		Status: &v1.PodSandboxStatus{
			Id:        id,
			Metadata:  mapper.getPodMetadata(ctx, id),
			State:     mapper.getPodState(ctx, id),
			CreatedAt: mapper.getTimeProperty(ctx, id, "creation_time[raw]"),
			Network:   mapper.getNetwork(ctx, id),
			Linux: &v1.LinuxPodSandboxStatus{
				Namespaces: &v1.Namespace{
					Options: &v1.NamespaceOption{
						Network: v1.NamespaceMode_POD,
						Pid:     v1.NamespaceMode_POD,
						Ipc:     v1.NamespaceMode_POD,
					},
				},
			},
			Labels:      mapper.getLabels(ctx, id, "LABEL"),
			Annotations: mapper.getLabels(ctx, id, "ANNOTATION"),
		},
	}, nil
}
func (mapper *PortodshimRuntimeMapper) PodSandboxStats(ctx context.Context, req *v1.PodSandboxStatsRequest) (*v1.PodSandboxStatsResponse, error) {
	zap.S().Debugf("call %s: %s", getCurrentFuncName(), req.GetPodSandboxId())

	id := req.GetPodSandboxId()

	return &v1.PodSandboxStatsResponse{
		Stats: mapper.getPodStats(ctx, id),
	}, nil
}
func (mapper *PortodshimRuntimeMapper) ListPodSandbox(ctx context.Context, req *v1.ListPodSandboxRequest) (*v1.ListPodSandboxResponse, error) {
	zap.S().Debugf("call %s: %s %s %s", getCurrentFuncName(), req.GetFilter().GetId(), req.GetFilter().GetState().GetState().String(), req.GetFilter().GetLabelSelector())

	portoClient := ctx.Value("portoClient").(API)

	targetId := req.GetFilter().GetId()
	targetState := req.GetFilter().GetState()
	targetLabels := req.GetFilter().GetLabelSelector()

	mask := "*"
	if targetId != "" {
		mask = targetId
	}

	response, err := portoClient.List1(mask)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	var items []*v1.PodSandbox
	for _, id := range response {
		// skip not k8s
		if ns := mapper.getValueForKubeLabel(ctx, id, "io.kubernetes.pod.namespace", "LABEL"); ns == "" {
			continue
		}

		// filtering
		state := mapper.getPodState(ctx, id)
		if targetState != nil && targetState.GetState() != state {
			continue
		}

		labels := mapper.getLabels(ctx, id, "LABEL")
		skip := false
		for targetLabel, targetValue := range targetLabels {
			if value, found := labels[targetLabel]; !found || value != targetValue {
				skip = true
				break
			}
		}

		if skip {
			continue
		}

		items = append(items, &v1.PodSandbox{
			Id:          id,
			Metadata:    mapper.getPodMetadata(ctx, id),
			State:       state,
			CreatedAt:   mapper.getTimeProperty(ctx, id, "creation_time[raw]"),
			Labels:      labels,
			Annotations: mapper.getLabels(ctx, id, "ANNOTATION"),
		})
	}

	return &v1.ListPodSandboxResponse{
		Items: items,
	}, nil
}
func (mapper *PortodshimRuntimeMapper) CreateContainer(ctx context.Context, req *v1.CreateContainerRequest) (*v1.CreateContainerResponse, error) {
	zap.S().Debugf("call %s: %s/%s", getCurrentFuncName(), req.GetPodSandboxId(), req.GetConfig().GetMetadata().GetName())

	portoClient := ctx.Value("portoClient").(API)

	// <id> := <podId>/<containerId>
	podId := req.GetPodSandboxId()
	containerId := mapper.createId(req.GetConfig().GetMetadata().GetName())
	id := fmt.Sprintf("%s/%s", podId, containerId)
	image := req.GetConfig().GetImage().GetImage()

	// TODO: УБрать заглушку
	if strings.HasPrefix(image, "k8s.gcr.io/") {
		image = "ubuntu:xenial"
	}

	err := portoClient.Create(id)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// volume creating
	config := map[string]string{
		"backend":    "overlay",
		"containers": id,
		"layers":     image,
	}
	rootPath := VolumesPath + "/" + id
	err = os.Mkdir(rootPath, 0755)
	if err != nil {
		if os.IsExist(err) {
			zap.S().Warnf("%s: directory already exists: %s", getCurrentFuncName(), rootPath)
		} else {
			_ = portoClient.Destroy(id)
			return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
		}
	}

	_, err = portoClient.CreateVolume(rootPath, config)
	if err != nil {
		_ = portoClient.Destroy(id)
		_ = os.RemoveAll(rootPath)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	err = portoClient.SetProperty(id, "root", "/"+containerId)
	if err != nil {
		_ = portoClient.Destroy(id)
		_ = os.RemoveAll(rootPath)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// labels and annotations
	labels := req.GetConfig().GetLabels()
	if labels == nil {
		labels = make(map[string]string)
	}
	labels["attempt"] = fmt.Sprint(req.GetConfig().GetMetadata().GetAttempt())

	mapper.setLabels(ctx, id, req.GetConfig().GetLabels(), "LABEL")
	mapper.setLabels(ctx, id, req.GetConfig().GetAnnotations(), "ANNOTATION")

	// command
	cmd := strings.Join(req.GetConfig().GetCommand(), " ")
	if cmd == "" {
		cmd = "sleep inf"
	}
	err = portoClient.SetProperty(id, "command", cmd)
	if err != nil {
		zap.S().Warnf("%s: %v", getCurrentFuncName(), err)
	}

	return &v1.CreateContainerResponse{
		ContainerId: id,
	}, nil
}
func (mapper *PortodshimRuntimeMapper) StartContainer(ctx context.Context, req *v1.StartContainerRequest) (*v1.StartContainerResponse, error) {
	zap.S().Debugf("call %s: %s", getCurrentFuncName(), req.GetContainerId())

	portoClient := ctx.Value("portoClient").(API)

	id := req.GetContainerId()
	if !mapper.isContainer(id) {
		return nil, fmt.Errorf("%s: %s specified ID belongs to pod", getCurrentFuncName(), id)
	}

	err := portoClient.Start(id)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	return &v1.StartContainerResponse{}, nil
}
func (mapper *PortodshimRuntimeMapper) StopContainer(ctx context.Context, req *v1.StopContainerRequest) (*v1.StopContainerResponse, error) {
	zap.S().Debugf("call %s: %s", getCurrentFuncName(), req.GetContainerId())

	portoClient := ctx.Value("portoClient").(API)

	id := req.GetContainerId()
	if !mapper.isContainer(id) {
		return nil, fmt.Errorf("%s: %s specified ID belongs to pod", getCurrentFuncName(), id)
	}

	if state := mapper.getStringProperty(ctx, id, "state"); state == "running" {
		err := portoClient.Kill(id, 15)
		if err != nil {
			return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
		}
	}

	return &v1.StopContainerResponse{}, nil
}
func (mapper *PortodshimRuntimeMapper) RemoveContainer(ctx context.Context, req *v1.RemoveContainerRequest) (*v1.RemoveContainerResponse, error) {
	zap.S().Debugf("call %s: %s", getCurrentFuncName(), req.GetContainerId())

	portoClient := ctx.Value("portoClient").(API)

	id := req.GetContainerId()
	if !mapper.isContainer(id) {
		return nil, fmt.Errorf("%s: %s specified ID belongs to pod", getCurrentFuncName(), id)
	}

	err := portoClient.Destroy(id)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	rootPath := VolumesPath + "/" + id
	err = os.RemoveAll(rootPath)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	return &v1.RemoveContainerResponse{}, nil
}
func (mapper *PortodshimRuntimeMapper) ListContainers(ctx context.Context, req *v1.ListContainersRequest) (*v1.ListContainersResponse, error) {
	zap.S().Debugf("call %s: %s %s %s %s", getCurrentFuncName(), req.GetFilter().GetId(), req.GetFilter().GetState().GetState().String(), req.GetFilter().GetPodSandboxId(), req.GetFilter().GetLabelSelector())

	portoClient := ctx.Value("portoClient").(API)
	targetId := req.GetFilter().GetId()
	targetState := req.GetFilter().GetState()
	targetPodSandboxId := req.GetFilter().GetPodSandboxId()
	targetLabels := req.GetFilter().GetLabelSelector()

	mask := ""
	if targetPodSandboxId != "" {
		mask = targetPodSandboxId + "/***"
	}
	if targetId != "" {
		mask = targetId
	}

	response, err := portoClient.List1(mask)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	var containers []*v1.Container
	for _, id := range response {
		// skip containers with level = 1
		if !mapper.isContainer(id) {
			continue
		}

		// skip not k8s
		if ns := mapper.getValueForKubeLabel(ctx, id, "io.kubernetes.pod.namespace", "LABEL"); ns == "" {
			continue
		}

		podId, _ := mapper.getPodAndContainer(id)

		// filtering
		state := mapper.getContainerState(ctx, id)
		if targetState != nil && targetState.GetState() != state {
			continue
		}

		labels := mapper.getLabels(ctx, id, "LABEL")
		skip := false
		for targetLabel, targetValue := range targetLabels {
			if value, found := labels[targetLabel]; !found || value != targetValue {
				skip = true
				break
			}
		}

		if skip {
			continue
		}

		image := mapper.getContainerImage(ctx, id)

		containers = append(containers, &v1.Container{
			Id:           id,
			PodSandboxId: podId,
			Metadata:     mapper.getContainerMetadata(ctx, id),
			Image: &v1.ImageSpec{
				Image: image,
			},
			ImageRef:    image,
			State:       mapper.getContainerState(ctx, id),
			CreatedAt:   mapper.getTimeProperty(ctx, id, "creation_time[raw]"),
			Labels:      labels,
			Annotations: mapper.getLabels(ctx, id, "ANNOTATION"),
		})
	}

	return &v1.ListContainersResponse{
		Containers: containers,
	}, nil
}
func (mapper *PortodshimRuntimeMapper) ContainerStatus(ctx context.Context, req *v1.ContainerStatusRequest) (*v1.ContainerStatusResponse, error) {
	zap.S().Debugf("call %s: %s", getCurrentFuncName(), req.GetContainerId())

	id := req.GetContainerId()
	if !mapper.isContainer(id) {
		return nil, fmt.Errorf("%s: specified ID belongs to pod", getCurrentFuncName())
	}

	image := mapper.getContainerImage(ctx, id)

	return &v1.ContainerStatusResponse{
		Status: &v1.ContainerStatus{
			Id:         id,
			Metadata:   mapper.getContainerMetadata(ctx, id),
			State:      mapper.getContainerState(ctx, id),
			CreatedAt:  mapper.getTimeProperty(ctx, id, "creation_time[raw]"),
			StartedAt:  mapper.getTimeProperty(ctx, id, "start_time[raw]"),
			FinishedAt: mapper.getTimeProperty(ctx, id, "death_time[raw]"),
			ExitCode:   int32(mapper.getIntProperty(ctx, id, "exit_code")),
			Image: &v1.ImageSpec{
				Image: image,
			},
			ImageRef:    image,
			Labels:      mapper.getLabels(ctx, id, "LABEL"),
			Annotations: mapper.getLabels(ctx, id, "ANNOTATION"),
		},
	}, nil
}
func (mapper *PortodshimRuntimeMapper) UpdateContainerResources(ctx context.Context, req *v1.UpdateContainerResourcesRequest) (*v1.UpdateContainerResourcesResponse, error) {
	zap.S().Debugf("call %s", getCurrentFuncName())

	return nil, fmt.Errorf("not implemented UpdateContainerResources")
}
func (mapper *PortodshimRuntimeMapper) ReopenContainerLog(ctx context.Context, req *v1.ReopenContainerLogRequest) (*v1.ReopenContainerLogResponse, error) {
	zap.S().Debugf("call %s", getCurrentFuncName())

	// return nil, fmt.Errorf("not implemented ReopenContainerLog")
	// TODO: реализовать ReopenContainerLog и убрать заглушку
	return &v1.ReopenContainerLogResponse{}, nil
}
func (mapper *PortodshimRuntimeMapper) ExecSync(ctx context.Context, req *v1.ExecSyncRequest) (*v1.ExecSyncResponse, error) {
	zap.S().Debugf("call %s", getCurrentFuncName())

	return nil, fmt.Errorf("not implemented ExecSync")
}
func (mapper *PortodshimRuntimeMapper) Exec(ctx context.Context, req *v1.ExecRequest) (*v1.ExecResponse, error) {
	zap.S().Debugf("call %s", getCurrentFuncName())

	return nil, fmt.Errorf("not implemented Exec")
}
func (mapper *PortodshimRuntimeMapper) Attach(ctx context.Context, req *v1.AttachRequest) (*v1.AttachResponse, error) {
	zap.S().Debugf("call %s", getCurrentFuncName())

	return nil, fmt.Errorf("not implemented Attach")
}
func (mapper *PortodshimRuntimeMapper) PortForward(ctx context.Context, req *v1.PortForwardRequest) (*v1.PortForwardResponse, error) {
	zap.S().Debugf("call %s", getCurrentFuncName())

	return nil, fmt.Errorf("not implemented PortForward")
}
func (mapper *PortodshimRuntimeMapper) ContainerStats(ctx context.Context, req *v1.ContainerStatsRequest) (*v1.ContainerStatsResponse, error) {
	zap.S().Debugf("call %s: %s", getCurrentFuncName(), req.GetContainerId())

	id := req.GetContainerId()
	if !mapper.isContainer(id) {
		return nil, fmt.Errorf("%s: specified ID belongs to pod", getCurrentFuncName())
	}

	return &v1.ContainerStatsResponse{
		Stats: mapper.getContainerStats(ctx, id),
	}, nil
}
func (mapper *PortodshimRuntimeMapper) ListContainerStats(ctx context.Context, req *v1.ListContainerStatsRequest) (*v1.ListContainerStatsResponse, error) {
	zap.S().Debugf("call %s: %s %s %s", getCurrentFuncName(), req.GetFilter().GetId(), req.GetFilter().GetPodSandboxId(), req.GetFilter().GetLabelSelector())

	portoClient := ctx.Value("portoClient").(API)

	targetId := req.GetFilter().GetId()
	targetPodSandboxId := req.GetFilter().GetPodSandboxId()
	targetLabels := req.GetFilter().GetLabelSelector()

	mask := ""
	if targetPodSandboxId != "" {
		mask = targetPodSandboxId + "/***"
	}
	if targetId != "" {
		mask = targetId
	}

	response, err := portoClient.List1(mask)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	var stats []*v1.ContainerStats
	for _, id := range response {
		// skip containers with level = 1
		if !mapper.isContainer(id) {
			continue
		}

		// skip not k8s
		if ns := mapper.getValueForKubeLabel(ctx, id, "io.kubernetes.pod.namespace", "LABEL"); ns == "" {
			continue
		}

		labels := mapper.getLabels(ctx, id, "LABEL")
		skip := false
		for targetLabel, targetValue := range targetLabels {
			if value, found := labels[targetLabel]; !found || value != targetValue {
				skip = true
				break
			}
		}

		if skip {
			continue
		}

		stats = append(stats, mapper.getContainerStats(ctx, id))
	}

	return &v1.ListContainerStatsResponse{
		Stats: stats,
	}, nil
}
func (mapper *PortodshimRuntimeMapper) ListPodSandboxStats(ctx context.Context, req *v1.ListPodSandboxStatsRequest) (*v1.ListPodSandboxStatsResponse, error) {
	zap.S().Debugf("call %s: %s %s", getCurrentFuncName(), req.GetFilter().GetId(), req.GetFilter().GetLabelSelector())

	portoClient := ctx.Value("portoClient").(API)

	targetId := req.GetFilter().GetId()
	targetLabels := req.GetFilter().GetLabelSelector()

	mask := "*"
	if targetId != "" {
		mask = targetId
	}

	response, err := portoClient.List1(mask)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	var stats []*v1.PodSandboxStats
	for _, id := range response {
		// skip not k8s
		if ns := mapper.getValueForKubeLabel(ctx, id, "io.kubernetes.pod.namespace", "LABEL"); ns == "" {
			continue
		}

		labels := mapper.getLabels(ctx, id, "LABEL")
		skip := false
		for targetLabel, targetValue := range targetLabels {
			if value, found := labels[targetLabel]; !found || value != targetValue {
				skip = true
				break
			}
		}

		if skip {
			continue
		}

		stats = append(stats, mapper.getPodStats(ctx, id))
	}

	return &v1.ListPodSandboxStatsResponse{
		Stats: stats,
	}, nil
}
func (mapper *PortodshimRuntimeMapper) UpdateRuntimeConfig(ctx context.Context, req *v1.UpdateRuntimeConfigRequest) (*v1.UpdateRuntimeConfigResponse, error) {
	zap.S().Debugf("call %s", getCurrentFuncName())

	return nil, fmt.Errorf("not implemented UpdateRuntimeConfig")
}
func (mapper *PortodshimRuntimeMapper) Status(ctx context.Context, req *v1.StatusRequest) (*v1.StatusResponse, error) {
	zap.S().Debugf("call %s", getCurrentFuncName())

	portoClient := ctx.Value("portoClient").(API)

	if _, _, err := portoClient.GetVersion(); err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	var conditions []*v1.RuntimeCondition
	conditions = append(conditions, &v1.RuntimeCondition{
		Type:   "RuntimeReady",
		Status: true,
	})
	conditions = append(conditions, &v1.RuntimeCondition{
		Type:   "NetworkReady",
		Status: true,
	})
	return &v1.StatusResponse{
		Status: &v1.RuntimeStatus{
			Conditions: conditions,
		},
	}, nil
}
