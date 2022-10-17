package main

import (
	"context"
	"encoding/base64"
	"fmt"
	"math/rand"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/containerd/containerd/pkg/netns"
	cni "github.com/containerd/go-cni"
	"github.com/yandex/porto/src/api/go/porto"
	"github.com/yandex/porto/src/api/go/porto/pkg/rpc"
	"go.uber.org/zap"
	v1 "k8s.io/cri-api/pkg/apis/runtime/v1"
)

const (
	runtimeName = "porto"
	pauseImage  = "k8s.gcr.io/pause:3.7"
	// loopback + default
	networkAttachCount = 2
	ifPrefixName       = "veth"
	defaultIfName      = "veth0"
)

type PortodshimRuntimeMapper struct {
	containerStateMap map[string]v1.ContainerState
	podStateMap       map[string]v1.PodSandboxState
	randGenerator     *rand.Rand
	netPlugin         cni.CNI
}

func NewPortodshimRuntimeMapper() (*PortodshimRuntimeMapper, error) {
	runtimeMapper := &PortodshimRuntimeMapper{}

	netPlugin, err := cni.New(cni.WithMinNetworkCount(networkAttachCount),
		cni.WithPluginConfDir(NetworkPluginConfDir),
		cni.WithPluginDir([]string{NetworkPluginBinDir}),
		cni.WithInterfacePrefix(ifPrefixName))
	if err != nil {
		return nil, fmt.Errorf("failed to initialize cni: %w", err)
	}
	if err = netPlugin.Load(cni.WithLoNetwork, cni.WithDefaultConf); err != nil {
		zap.S().Warnf("failed to load cni configuration: %v", err)
	} else {
		runtimeMapper.netPlugin = netPlugin
	}

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

	return runtimeMapper, nil
}

// INTERNAL
func (mapper *PortodshimRuntimeMapper) getContainerState(ctx context.Context, id string) v1.ContainerState {
	portoClient := ctx.Value("portoClient").(porto.API)

	state, err := portoClient.GetProperty(id, "state")
	if err != nil {
		return v1.ContainerState_CONTAINER_UNKNOWN
	}

	return mapper.containerStateMap[state]
}

func (mapper *PortodshimRuntimeMapper) getPodState(ctx context.Context, id string) v1.PodSandboxState {
	portoClient := ctx.Value("portoClient").(porto.API)

	state, err := portoClient.GetProperty(id, "state")
	if err != nil {
		return v1.PodSandboxState_SANDBOX_NOTREADY
	}

	return mapper.podStateMap[state]
}

func (mapper *PortodshimRuntimeMapper) createId(name string) string {
	length := 26
	if len(name) < length {
		length = len(name)
	}
	// max length of return value is 26 + 1 + 4 = 31, so container id <= 63
	return fmt.Sprintf("%s-%x", name[:length], mapper.randGenerator.Uint32()%65536)
}

func (mapper *PortodshimRuntimeMapper) prepareContainerResources(ctx context.Context, id string, cfg *v1.LinuxContainerResources) error {
	portoClient := ctx.Value("portoClient").(porto.API)

	if cfg == nil {
		return nil
	}

	// cpu
	if err := portoClient.SetProperty(id, "cpu_limit", fmt.Sprintf("%fc", float64(cfg.CpuQuota)/100000)); err != nil {
		return fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	if err := portoClient.SetProperty(id, "cpu_guarantee", fmt.Sprintf("%fc", float64(cfg.CpuQuota)/100000)); err != nil {
		return fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// memory
	if err := portoClient.SetProperty(id, "memory_limit", strconv.FormatInt(cfg.MemoryLimitInBytes, 10)); err != nil {
		return fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	if err := portoClient.SetProperty(id, "memory_guarantee", strconv.FormatInt(cfg.MemoryLimitInBytes, 10)); err != nil {
		return fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	return nil
}

func (mapper *PortodshimRuntimeMapper) prepareContainerNetwork(ctx context.Context, id string, cfg *v1.PodSandboxConfig) error {
	portoClient := ctx.Value("portoClient").(porto.API)

	nsOpts := cfg.GetLinux().GetSecurityContext().GetNamespaceOptions()
	if nsOpts.Network == v1.NamespaceMode_NODE {
		return nil
	}

	if mapper.netPlugin == nil {
		return fmt.Errorf("cni wasn't initialized")
	}

	netnsPath, err := netns.NewNetNS(NetnsDir)
	if err != nil {
		return fmt.Errorf("failed to create network namespace, pod %s: %w", id, err)
	}
	cniLabels := cni.WithLabels(makeCniLabels(id, cfg))
	result, err := mapper.netPlugin.Setup(ctx, id, netnsPath.GetPath(), cniLabels)
	if err != nil {
		return err
	}
	addrs := []string{}
	for _, ip := range result.Interfaces[defaultIfName].IPConfigs {
		ipv6 := ip.IP.To16()
		if ipv6 != nil {
			addrs = append(addrs, fmt.Sprintf("%s %s", defaultIfName, ipv6))
		}
	}

	// hostname
	if err = portoClient.SetProperty(id, "hostname", cfg.GetHostname()); err != nil {
		return fmt.Errorf("failed set porto prop hostname, pod %s: %w", id, err)
	}

	// net
	err = portoClient.SetProperty(id, "net", fmt.Sprintf("netns %s", filepath.Base(netnsPath.GetPath())))
	if err != nil {
		return fmt.Errorf("failed set porto prop net, pod %s: %w", id, err)
	}
	err = portoClient.SetProperty(id, "ip", strings.Join(addrs, ";"))
	if err != nil {
		return fmt.Errorf("failed set porto prop ip, pod %s: %w", id, err)
	}

	// sysctl
	sysctls := []string{}
	for k, v := range cfg.GetLinux().GetSysctls() {
		sysctls = append(sysctls, fmt.Sprintf("%s:%s", k, v))
	}
	err = portoClient.SetProperty(id, "sysctl", strings.Join(sysctls, ";"))
	if err != nil {
		return fmt.Errorf("failed set porto prop sysctl, pod %s: %w", id, err)
	}

	return nil
}

func (mapper *PortodshimRuntimeMapper) prepareContainerImage(ctx context.Context, id string, imageName string) (*rpc.TDockerImage, error) {
	portoClient := ctx.Value("portoClient").(porto.API)

	image, err := portoClient.DockerImageStatus(imageName, "")
	if err != nil {
		if err.(*porto.Error).Errno == rpc.EError_DockerImageNotFound {
			image, err = portoClient.PullDockerImage(imageName, "", "", "", "")
			if err != nil {
				return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
			}
		} else {
			return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
		}
	}
	return image, nil
}

func (mapper *PortodshimRuntimeMapper) prepareContainerCommand(ctx context.Context, id string, cfgCmd []string, cfgArgs []string, imgCmd []string) error {
	portoClient := ctx.Value("portoClient").(porto.API)

	cmd := imgCmd
	if len(cfgCmd) > 0 {
		cmd = cfgCmd
	}
	cmd = append(cmd, cfgArgs...)

	return portoClient.SetProperty(id, "command", strings.Join(cmd, " "))
}

func (mapper *PortodshimRuntimeMapper) prepareContainerEnv(ctx context.Context, id string, env []*v1.KeyValue, image *rpc.TDockerImage) error {
	portoClient := ctx.Value("portoClient").(porto.API)

	envProp := []string{image.GetEnv()}
	for _, i := range env {
		envProp = append(envProp, fmt.Sprintf("%s=%s", i.Key, i.Value))
	}
	return portoClient.SetProperty(id, "env", strings.Join(envProp, ";"))
}

func (mapper *PortodshimRuntimeMapper) prepareContainerRoot(ctx context.Context, id string, rootPath string, image string) (string, error) {
	portoClient := ctx.Value("portoClient").(porto.API)

	rootAbsPath := filepath.Join(VolumesDir, id)
	if rootPath == "" {
		rootPath = rootAbsPath
	}
	err := os.Mkdir(rootAbsPath, 0755)
	if err != nil {
		if os.IsExist(err) {
			zap.S().Warnf("%s: directory already exists: %s", getCurrentFuncName(), rootPath)
		} else {
			return rootAbsPath, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
		}
	}
	_, err = portoClient.CreateVolume(rootAbsPath, map[string]string{
		"containers": id,
		"image":      image,
		"backend":    "overlay",
	})
	if err != nil {
		_ = os.RemoveAll(rootAbsPath)
		return rootAbsPath, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	if err = portoClient.SetProperty(id, "root", rootPath); err != nil {
		_ = os.RemoveAll(rootAbsPath)
		return rootAbsPath, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	return rootAbsPath, nil
}

func (mapper *PortodshimRuntimeMapper) prepareContainerMounts(ctx context.Context, id string, mounts []*v1.Mount) error {
	portoClient := ctx.Value("portoClient").(porto.API)

	for _, mount := range mounts {
		if mount.ContainerPath == "/dev/termination-log" {
			continue
		}
		// TODO: durty hack
		if mount.ContainerPath == "/var/run/secrets/kubernetes.io/serviceaccount" {
			for {
				_, err := os.Stat(mount.HostPath + "/ca.crt")
				if err == nil {
					break
				}
				zap.S().Warnf("%s waiting for a %s", id, mount.HostPath)
				time.Sleep(1000)
			}
		}
		_, err := portoClient.CreateVolume(mount.HostPath, map[string]string{
			"backend": "bind",
			"storage": mount.HostPath,
		})
		if err != nil && err.(*porto.Error).Errno != rpc.EError_VolumeAlreadyExists {
			return fmt.Errorf("%s: %s %s %v", getCurrentFuncName(), id, mount.HostPath, err)
		}
		err = portoClient.LinkVolume(mount.HostPath, id, mount.ContainerPath, false, mount.Readonly)
		if err != nil {
			return fmt.Errorf("%s: %s %s %s %v", getCurrentFuncName(), id, mount.HostPath, mount.ContainerPath, err)
		}
		err = portoClient.UnlinkVolume(mount.HostPath, "/", "")
		if err != nil && err.(*porto.Error).Errno != rpc.EError_VolumeNotLinked {
			return fmt.Errorf("%s: %s %s %v", getCurrentFuncName(), id, mount.HostPath, err)
		}
	}
	return nil
}

func (mapper *PortodshimRuntimeMapper) prepareContainerResolvConf(ctx context.Context, id string, cfg *v1.DNSConfig) error {
	portoClient := ctx.Value("portoClient").(porto.API)

	if cfg != nil {
		resolvConf := []string{}
		for _, i := range cfg.GetServers() {
			resolvConf = append(resolvConf, fmt.Sprintf("%s %s", "nameserver", i))
		}
		resolvConf = append(resolvConf, fmt.Sprintf("%s %s", "search", strings.Join(cfg.GetSearches(), " ")))
		resolvConf = append(resolvConf, fmt.Sprintf("%s %s\n", "options", strings.Join(cfg.GetOptions(), " ")))
		return portoClient.SetProperty(id, "resolv_conf", strings.Join(resolvConf, ";"))
	}

	return nil
}

func getContainerNetNS(ctx context.Context, id string) (string, error) {
	portoClient := ctx.Value("portoClient").(porto.API)

	netProp, err := portoClient.GetProperty(id, "net")
	if err != nil {
		return "", err
	}
	return parsePropertyNetNS(netProp), nil
}

func getContainerNetNSMode(ctx context.Context, id string) (v1.NamespaceMode, error) {
	netNSMode := v1.NamespaceMode_NODE
	netNSProp, err := getContainerNetNS(ctx, id)
	if err != nil {
		return netNSMode, err
	}
	if netNSProp != "" {
		netNSMode = v1.NamespaceMode_POD
	}
	return netNSMode, nil
}

func getPodAndContainer(id string) (string, string) {
	// <id> := <podId>/<containerId>
	podId := strings.Split(id, "/")[0]
	containerId := ""
	if len(podId) != len(id) {
		containerId = id[len(podId)+1:]
	}

	return podId, containerId
}

func isContainer(id string) bool {
	_, containerId := getPodAndContainer(id)
	return containerId != ""
}

func convertBase64(src string, encode bool) string {
	if encode {
		return base64.RawStdEncoding.EncodeToString([]byte(src))
	}

	dst, err := base64.RawStdEncoding.DecodeString(src)
	if err != nil {
		return src
	}

	return string(dst)
}

func convertLabel(src string, toPorto bool, prefix string) string {
	dst := src
	if toPorto {
		dst = convertBase64(dst, true)
		if prefix != "" {
			dst = prefix + "." + dst
		}
	} else {
		if prefix != "" {
			dst = strings.TrimPrefix(dst, prefix+".")
		}
		dst = convertBase64(dst, false)
	}

	return dst
}

func setLabels(ctx context.Context, id string, labels map[string]string, prefix string) error {
	portoClient := ctx.Value("portoClient").(porto.API)

	labelsString := ""
	for label, value := range labels {
		labelsString += fmt.Sprintf("%s:%s;", convertLabel(label, true, prefix), convertLabel(value, true, ""))
	}

	return portoClient.SetProperty(id, "labels", labelsString)
}

func getLabels(ctx context.Context, id string, prefix string) map[string]string {
	portoClient := ctx.Value("portoClient").(porto.API)

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
			result[convertLabel(label, false, prefix)] = convertLabel(value, false, "")
		}
	}

	return result
}

func getValueForKubeLabel(ctx context.Context, id string, label string, prefix string) string {
	return convertLabel(getStringProperty(ctx, id, fmt.Sprintf("labels[%s]", convertLabel(label, true, prefix))), false, "")
}

func getTimeProperty(ctx context.Context, id string, property string) int64 {
	return time.Unix(int64(getUintProperty(ctx, id, property)), 0).UnixNano()
}

func getUintProperty(ctx context.Context, id string, property string) uint64 {
	portoClient := ctx.Value("portoClient").(porto.API)

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

func getIntProperty(ctx context.Context, id string, property string) int64 {
	portoClient := ctx.Value("portoClient").(porto.API)

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

func getStringProperty(ctx context.Context, id string, property string) string {
	portoClient := ctx.Value("portoClient").(porto.API)

	value, err := portoClient.GetProperty(id, property)
	if err != nil {
		return ""
	}

	return value
}

func getPodMetadata(ctx context.Context, id string) *v1.PodSandboxMetadata {
	labels := getLabels(ctx, id, "LABEL")
	attempt, _ := strconv.ParseUint(labels["attempt"], 10, 64)

	return &v1.PodSandboxMetadata{
		Name:      labels["io.kubernetes.pod.name"],
		Uid:       labels["io.kubernetes.pod.uid"],
		Namespace: labels["io.kubernetes.pod.namespace"],
		Attempt:   uint32(attempt),
	}
}

func getPodStats(ctx context.Context, id string) *v1.PodSandboxStats {
	portoClient := ctx.Value("portoClient").(porto.API)

	cpu := getUintProperty(ctx, id, "cpu_usage")
	timestamp := time.Now().UnixNano()

	response, err := portoClient.List1(id + "/***")
	if err != nil {
		zap.S().Warnf("%s: %v", getCurrentFuncName(), err)
		return nil
	}

	var stats []*v1.ContainerStats
	for _, ctrId := range response {
		stats = append(stats, getContainerStats(ctx, ctrId))
	}

	// TODO: Заполнить оставшиеся метрики
	return &v1.PodSandboxStats{
		Attributes: &v1.PodSandboxAttributes{
			Id:          id,
			Metadata:    getPodMetadata(ctx, id),
			Labels:      getLabels(ctx, id, "LABEL"),
			Annotations: getLabels(ctx, id, "ANNOTATION"),
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
				UsageBytes:      &v1.UInt64Value{Value: getUintProperty(ctx, id, "memory_usage")},
				RssBytes:        &v1.UInt64Value{Value: 0},
				PageFaults:      &v1.UInt64Value{Value: getUintProperty(ctx, id, "minor_faults")},
				MajorPageFaults: &v1.UInt64Value{Value: getUintProperty(ctx, id, "major_faults")},
			},
			Network: &v1.NetworkUsage{
				Timestamp: timestamp,
				DefaultInterface: &v1.NetworkInterfaceUsage{
					Name:     defaultIfName,
					RxBytes:  &v1.UInt64Value{Value: getUintProperty(ctx, id, "net_rx_bytes")},
					RxErrors: &v1.UInt64Value{Value: 0},
					TxBytes:  &v1.UInt64Value{Value: getUintProperty(ctx, id, "net_bytes")},
					TxErrors: &v1.UInt64Value{Value: 0},
				},
			},
			Process: &v1.ProcessUsage{
				Timestamp:    timestamp,
				ProcessCount: &v1.UInt64Value{Value: getUintProperty(ctx, id, "process_count")},
			},
			Containers: stats,
		},
		Windows: &v1.WindowsPodSandboxStats{},
	}
}

func getContainerMetadata(ctx context.Context, id string) *v1.ContainerMetadata {
	labels := getLabels(ctx, id, "LABEL")
	attempt, _ := strconv.ParseUint(labels["attempt"], 10, 64)

	return &v1.ContainerMetadata{
		Name:    labels["io.kubernetes.container.name"],
		Attempt: uint32(attempt),
	}
}

func getContainerStats(ctx context.Context, id string) *v1.ContainerStats {
	cpu := getUintProperty(ctx, id, "cpu_usage")
	timestamp := time.Now().UnixNano()

	// TODO: Заполнить оставшиеся метрики
	return &v1.ContainerStats{
		Attributes: &v1.ContainerAttributes{
			Id:          id,
			Metadata:    getContainerMetadata(ctx, id),
			Labels:      getLabels(ctx, id, "LABEL"),
			Annotations: getLabels(ctx, id, "ANNOTATION"),
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
			UsageBytes:      &v1.UInt64Value{Value: getUintProperty(ctx, id, "memory_usage")},
			RssBytes:        &v1.UInt64Value{Value: 0},
			PageFaults:      &v1.UInt64Value{Value: getUintProperty(ctx, id, "minor_faults")},
			MajorPageFaults: &v1.UInt64Value{Value: getUintProperty(ctx, id, "major_faults")},
		},
		WritableLayer: &v1.FilesystemUsage{
			Timestamp: timestamp,
			FsId: &v1.FilesystemIdentifier{
				Mountpoint: VolumesDir + "/" + id,
			},
			UsedBytes:  &v1.UInt64Value{Value: 0},
			InodesUsed: &v1.UInt64Value{Value: 0},
		},
	}
}

func getContainerImage(ctx context.Context, id string) string {
	portoClient := ctx.Value("portoClient").(porto.API)

	if !isContainer(id) {
		return ""
	}

	imageDescriptions, err := portoClient.ListVolumes(VolumesDir+"/"+id, id)
	if err != nil {
		zap.S().Warnf("%s: %v", getCurrentFuncName(), err)
		return ""
	}
	return imageDescriptions[0].Properties["image"]
}

func getPodSandboxNetworkStatus(ctx context.Context, id string) *v1.PodSandboxNetworkStatus {
	portoClient := ctx.Value("portoClient").(porto.API)

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

func makeCniLabels(id string, config *v1.PodSandboxConfig) map[string]string {
	return map[string]string{
		"PrjID":         config.GetAnnotations()["PrjID"],
		"IgnoreUnknown": "1",
	}
}

func parsePropertyNetNS(prop string) string {
	netNSL := strings.Fields(prop)
	if netNSL[0] == "netns" && len(netNSL) > 1 {
		return netNSL[1]
	}
	return ""
}

// RUNTIME SERVICE INTERFACE
func (mapper *PortodshimRuntimeMapper) Version(ctx context.Context, req *v1.VersionRequest) (*v1.VersionResponse, error) {
	zap.S().Debugf("call %s", getCurrentFuncName())

	portoClient := ctx.Value("portoClient").(porto.API)

	tag, _, err := portoClient.GetVersion()
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	// TODO: temprorary use tag as a RuntimeApiVersion
	return &v1.VersionResponse{
		Version:           req.GetVersion(),
		RuntimeName:       runtimeName,
		RuntimeVersion:    tag,
		RuntimeApiVersion: tag,
	}, nil
}

func (mapper *PortodshimRuntimeMapper) RunPodSandbox(ctx context.Context, req *v1.RunPodSandboxRequest) (*v1.RunPodSandboxResponse, error) {
	zap.S().Debugf("call %s: %+v", getCurrentFuncName(), req.GetConfig())

	portoClient := ctx.Value("portoClient").(porto.API)

	id := mapper.createId(req.GetConfig().GetMetadata().GetName())
	err := portoClient.Create(id)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// get image
	image, err := mapper.prepareContainerImage(ctx, id, pauseImage)
	if err != nil {
		_ = portoClient.Destroy(id)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// command + args
	if err = mapper.prepareContainerCommand(ctx, id, []string{}, []string{}, []string{image.GetCommand()}); err != nil {
		_ = portoClient.Destroy(id)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// env
	if err = mapper.prepareContainerEnv(ctx, id, []*v1.KeyValue{}, image); err != nil {
		_ = portoClient.Destroy(id)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// resources
	res := req.GetConfig().GetLinux().GetResources()
	if res != nil {
		if err = mapper.prepareContainerResources(ctx, id, res); err != nil {
			_ = portoClient.Destroy(id)
			return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
		}
	}

	// labels and annotations
	labels := req.GetConfig().GetLabels()
	if labels == nil {
		labels = make(map[string]string)
	}
	if _, found := labels["io.kubernetes.pod.namespace"]; !found {
		labels["io.kubernetes.pod.namespace"] = req.GetConfig().GetMetadata().GetNamespace()
	}
	labels["attempt"] = fmt.Sprint(req.GetConfig().GetMetadata().GetAttempt())
	if err = setLabels(ctx, id, labels, "LABEL"); err != nil {
		_ = portoClient.Destroy(id)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	if err = setLabels(ctx, id, req.GetConfig().GetAnnotations(), "ANNOTATION"); err != nil {
		_ = portoClient.Destroy(id)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// root
	rootPath, err := mapper.prepareContainerRoot(ctx, id, "", pauseImage)
	if err != nil {
		_ = portoClient.Destroy(id)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// network
	cfg := req.GetConfig()
	if cfg != nil && cfg.GetLinux() != nil {
		if err = mapper.prepareContainerNetwork(ctx, id, cfg); err != nil {
			_ = portoClient.Destroy(id)
			_ = os.RemoveAll(rootPath)
			return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
		}
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

	portoClient := ctx.Value("portoClient").(porto.API)

	id := req.GetPodSandboxId()

	if state := getStringProperty(ctx, id, "state"); state == "running" {
		err := portoClient.Kill(id, 15)
		if err != nil {
			return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
		}
	}

	return &v1.StopPodSandboxResponse{}, nil
}

func (mapper *PortodshimRuntimeMapper) RemovePodSandbox(ctx context.Context, req *v1.RemovePodSandboxRequest) (*v1.RemovePodSandboxResponse, error) {
	zap.S().Debugf("call %s: %s", getCurrentFuncName(), req.GetPodSandboxId())

	portoClient := ctx.Value("portoClient").(porto.API)

	id := req.GetPodSandboxId()
	netProp, err := portoClient.GetProperty(id, "net")
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	if err = portoClient.Destroy(id); err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	rootPath := VolumesDir + "/" + id
	if err = os.RemoveAll(rootPath); err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// removes the network from the pod
	netnsProp := parsePropertyNetNS(netProp)
	if netnsProp == "" {
		return nil, fmt.Errorf("%s: %s", getCurrentFuncName(), "netns property hasn't been set")
	}
	netnsPath := netns.LoadNetNS(filepath.Join(NetnsDir, netnsProp))
	if err = mapper.netPlugin.Remove(ctx, id, netnsPath.GetPath(), cni.WithLabels(map[string]string{})); err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	if err = netnsPath.Remove(); err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	return &v1.RemovePodSandboxResponse{}, nil
}

func (mapper *PortodshimRuntimeMapper) PodSandboxStatus(ctx context.Context, req *v1.PodSandboxStatusRequest) (*v1.PodSandboxStatusResponse, error) {
	zap.S().Debugf("call %s: %s", getCurrentFuncName(), req.GetPodSandboxId())

	id := req.GetPodSandboxId()

	netNSMode, err := getContainerNetNSMode(ctx, id)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	resp := &v1.PodSandboxStatusResponse{
		Status: &v1.PodSandboxStatus{
			Id:        id,
			Metadata:  getPodMetadata(ctx, id),
			State:     mapper.getPodState(ctx, id),
			CreatedAt: getTimeProperty(ctx, id, "creation_time[raw]"),
			Network:   getPodSandboxNetworkStatus(ctx, id),
			Linux: &v1.LinuxPodSandboxStatus{
				Namespaces: &v1.Namespace{
					Options: &v1.NamespaceOption{
						Network: netNSMode,
						Pid:     v1.NamespaceMode_POD,
						Ipc:     v1.NamespaceMode_POD,
					},
				},
			},
			Labels:      getLabels(ctx, id, "LABEL"),
			Annotations: getLabels(ctx, id, "ANNOTATION"),
		},
	}
	zap.S().Debugf("resp %s: %+v", getCurrentFuncName(), resp)
	return resp, nil
}

func (mapper *PortodshimRuntimeMapper) PodSandboxStats(ctx context.Context, req *v1.PodSandboxStatsRequest) (*v1.PodSandboxStatsResponse, error) {
	zap.S().Debugf("call %s: %s", getCurrentFuncName(), req.GetPodSandboxId())

	id := req.GetPodSandboxId()

	return &v1.PodSandboxStatsResponse{
		Stats: getPodStats(ctx, id),
	}, nil
}

func (mapper *PortodshimRuntimeMapper) ListPodSandbox(ctx context.Context, req *v1.ListPodSandboxRequest) (*v1.ListPodSandboxResponse, error) {
	zap.S().Debugf("call %s: %s %s %s", getCurrentFuncName(), req.GetFilter().GetId(), req.GetFilter().GetState().GetState().String(), req.GetFilter().GetLabelSelector())

	portoClient := ctx.Value("portoClient").(porto.API)

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
		if ns := getValueForKubeLabel(ctx, id, "io.kubernetes.pod.namespace", "LABEL"); ns == "" {
			continue
		}

		// filtering
		state := mapper.getPodState(ctx, id)
		if targetState != nil && targetState.GetState() != state {
			continue
		}

		labels := getLabels(ctx, id, "LABEL")
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
			Metadata:    getPodMetadata(ctx, id),
			State:       state,
			CreatedAt:   getTimeProperty(ctx, id, "creation_time[raw]"),
			Labels:      labels,
			Annotations: getLabels(ctx, id, "ANNOTATION"),
		})
	}

	return &v1.ListPodSandboxResponse{
		Items: items,
	}, nil
}

func (mapper *PortodshimRuntimeMapper) CreateContainer(ctx context.Context, req *v1.CreateContainerRequest) (*v1.CreateContainerResponse, error) {
	zap.S().Debugf("call %s: %s %+v", getCurrentFuncName(), req.GetPodSandboxId(), req.GetConfig())

	portoClient := ctx.Value("portoClient").(porto.API)

	// <id> := <podId>/<containerId>
	podId := req.GetPodSandboxId()
	containerId := mapper.createId(req.GetConfig().GetMetadata().GetName())
	id := filepath.Join(podId, containerId)
	err := portoClient.Create(id)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// get image
	// TODO: define imageName in rpc.TDockerImage
	imageName := req.GetConfig().GetImage().GetImage()
	image, err := mapper.prepareContainerImage(ctx, id, imageName)
	if err != nil {
		_ = portoClient.Destroy(id)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// command + args
	if err = mapper.prepareContainerCommand(ctx, id, req.GetConfig().GetCommand(), req.GetConfig().GetArgs(), []string{image.GetCommand()}); err != nil {
		_ = portoClient.Destroy(id)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// env
	if err = mapper.prepareContainerEnv(ctx, id, req.GetConfig().GetEnvs(), image); err != nil {
		_ = portoClient.Destroy(id)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// labels and annotations
	labels := req.GetConfig().GetLabels()
	if labels == nil {
		labels = make(map[string]string)
	}
	labels["attempt"] = fmt.Sprint(req.GetConfig().GetMetadata().GetAttempt())
	labels["io.kubernetes.container.logpath"] = filepath.Join("/place/porto/", id, "/stdout")
	if err = setLabels(ctx, id, labels, "LABEL"); err != nil {
		_ = portoClient.Destroy(id)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)

	}
	if err = setLabels(ctx, id, req.GetConfig().GetAnnotations(), "ANNOTATION"); err != nil {
		_ = portoClient.Destroy(id)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// resolv.conf
	if err = mapper.prepareContainerResolvConf(ctx, id, req.GetSandboxConfig().GetDnsConfig()); err != nil {
		_ = portoClient.Destroy(id)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// root
	rootPath, err := mapper.prepareContainerRoot(ctx, id, "/"+containerId, imageName)
	if err != nil {
		_ = portoClient.Destroy(id)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// mounts
	if err = mapper.prepareContainerMounts(ctx, id, req.GetConfig().GetMounts()); err != nil {
		_ = portoClient.Destroy(id)
		_ = os.RemoveAll(rootPath)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	return &v1.CreateContainerResponse{
		ContainerId: id,
	}, nil
}

func (mapper *PortodshimRuntimeMapper) StartContainer(ctx context.Context, req *v1.StartContainerRequest) (*v1.StartContainerResponse, error) {
	zap.S().Debugf("call %s: %s", getCurrentFuncName(), req.GetContainerId())

	portoClient := ctx.Value("portoClient").(porto.API)

	id := req.GetContainerId()
	if !isContainer(id) {
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

	portoClient := ctx.Value("portoClient").(porto.API)

	id := req.GetContainerId()
	if !isContainer(id) {
		return nil, fmt.Errorf("%s: %s specified ID belongs to pod", getCurrentFuncName(), id)
	}

	if state := getStringProperty(ctx, id, "state"); state == "running" {
		err := portoClient.Kill(id, 15)
		if err != nil {
			return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
		}
	}

	return &v1.StopContainerResponse{}, nil
}

func (mapper *PortodshimRuntimeMapper) RemoveContainer(ctx context.Context, req *v1.RemoveContainerRequest) (*v1.RemoveContainerResponse, error) {
	zap.S().Debugf("call %s: %s", getCurrentFuncName(), req.GetContainerId())

	portoClient := ctx.Value("portoClient").(porto.API)

	id := req.GetContainerId()
	if !isContainer(id) {
		return nil, fmt.Errorf("%s: %s specified ID belongs to pod", getCurrentFuncName(), id)
	}

	err := portoClient.Destroy(id)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	rootPath := VolumesDir + "/" + id
	err = os.RemoveAll(rootPath)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	return &v1.RemoveContainerResponse{}, nil
}

func (mapper *PortodshimRuntimeMapper) ListContainers(ctx context.Context, req *v1.ListContainersRequest) (*v1.ListContainersResponse, error) {
	zap.S().Debugf("call %s: %s %s %s %s", getCurrentFuncName(), req.GetFilter().GetId(), req.GetFilter().GetState().GetState().String(), req.GetFilter().GetPodSandboxId(), req.GetFilter().GetLabelSelector())

	portoClient := ctx.Value("portoClient").(porto.API)
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
		if !isContainer(id) {
			continue
		}

		// skip not k8s
		if ns := getValueForKubeLabel(ctx, id, "io.kubernetes.pod.namespace", "LABEL"); ns == "" {
			continue
		}

		podId, _ := getPodAndContainer(id)

		// filtering
		state := mapper.getContainerState(ctx, id)
		if targetState != nil && targetState.GetState() != state {
			continue
		}

		labels := getLabels(ctx, id, "LABEL")
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

		image := getContainerImage(ctx, id)

		containers = append(containers, &v1.Container{
			Id:           id,
			PodSandboxId: podId,
			Metadata:     getContainerMetadata(ctx, id),
			Image: &v1.ImageSpec{
				Image: image,
			},
			ImageRef:    image,
			State:       mapper.getContainerState(ctx, id),
			CreatedAt:   getTimeProperty(ctx, id, "creation_time[raw]"),
			Labels:      labels,
			Annotations: getLabels(ctx, id, "ANNOTATION"),
		})
	}

	return &v1.ListContainersResponse{
		Containers: containers,
	}, nil
}

func (mapper *PortodshimRuntimeMapper) ContainerStatus(ctx context.Context, req *v1.ContainerStatusRequest) (*v1.ContainerStatusResponse, error) {
	zap.S().Debugf("call %s: %s", getCurrentFuncName(), req.GetContainerId())

	id := req.GetContainerId()
	if !isContainer(id) {
		return nil, fmt.Errorf("%s: specified ID belongs to pod", getCurrentFuncName())
	}

	image := getContainerImage(ctx, id)

	resp := &v1.ContainerStatusResponse{
		Status: &v1.ContainerStatus{
			Id:         id,
			Metadata:   getContainerMetadata(ctx, id),
			State:      mapper.getContainerState(ctx, id),
			CreatedAt:  getTimeProperty(ctx, id, "creation_time[raw]"),
			StartedAt:  getTimeProperty(ctx, id, "start_time[raw]"),
			FinishedAt: getTimeProperty(ctx, id, "death_time[raw]"),
			ExitCode:   int32(getIntProperty(ctx, id, "exit_code")),
			Image: &v1.ImageSpec{
				Image: image,
			},
			ImageRef:    image,
			Labels:      getLabels(ctx, id, "LABEL"),
			Annotations: getLabels(ctx, id, "ANNOTATION"),
			LogPath:     getValueForKubeLabel(ctx, id, "io.kubernetes.container.logpath", "LABEL"),
		},
	}
	zap.S().Debugf("resp %s: %+v", getCurrentFuncName(), resp)
	return resp, nil
}

func (mapper *PortodshimRuntimeMapper) UpdateContainerResources(ctx context.Context, req *v1.UpdateContainerResourcesRequest) (*v1.UpdateContainerResourcesResponse, error) {
	zap.S().Debugf("call %s", getCurrentFuncName())

	return nil, fmt.Errorf("not implemented UpdateContainerResources")
}

func (mapper *PortodshimRuntimeMapper) ReopenContainerLog(ctx context.Context, req *v1.ReopenContainerLogRequest) (*v1.ReopenContainerLogResponse, error) {
	zap.S().Debugf("call %s", getCurrentFuncName())

	// TODO: реализовать ReopenContainerLog
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
	if !isContainer(id) {
		return nil, fmt.Errorf("%s: specified ID belongs to pod", getCurrentFuncName())
	}

	return &v1.ContainerStatsResponse{
		Stats: getContainerStats(ctx, id),
	}, nil
}

func (mapper *PortodshimRuntimeMapper) ListContainerStats(ctx context.Context, req *v1.ListContainerStatsRequest) (*v1.ListContainerStatsResponse, error) {
	zap.S().Debugf("call %s: %s %s %s", getCurrentFuncName(), req.GetFilter().GetId(), req.GetFilter().GetPodSandboxId(), req.GetFilter().GetLabelSelector())

	portoClient := ctx.Value("portoClient").(porto.API)

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
		if !isContainer(id) {
			continue
		}

		// skip not k8s
		if ns := getValueForKubeLabel(ctx, id, "io.kubernetes.pod.namespace", "LABEL"); ns == "" {
			continue
		}

		labels := getLabels(ctx, id, "LABEL")
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

		stats = append(stats, getContainerStats(ctx, id))
	}

	return &v1.ListContainerStatsResponse{
		Stats: stats,
	}, nil
}

func (mapper *PortodshimRuntimeMapper) ListPodSandboxStats(ctx context.Context, req *v1.ListPodSandboxStatsRequest) (*v1.ListPodSandboxStatsResponse, error) {
	zap.S().Debugf("call %s: %s %s", getCurrentFuncName(), req.GetFilter().GetId(), req.GetFilter().GetLabelSelector())

	portoClient := ctx.Value("portoClient").(porto.API)

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
		if ns := getValueForKubeLabel(ctx, id, "io.kubernetes.pod.namespace", "LABEL"); ns == "" {
			continue
		}

		labels := getLabels(ctx, id, "LABEL")
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

		stats = append(stats, getPodStats(ctx, id))
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

	portoClient := ctx.Value("portoClient").(porto.API)

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
