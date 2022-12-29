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

	cri "github.com/containerd/containerd/pkg/cri/annotations"
	"github.com/containerd/containerd/pkg/netns"
	cni "github.com/containerd/go-cni"
	"github.com/yandex/porto/src/api/go/porto"
	"github.com/yandex/porto/src/api/go/porto/pkg/rpc"
	"go.uber.org/zap"
	v1 "k8s.io/cri-api/pkg/apis/runtime/v1"
	"k8s.io/kubernetes/pkg/kubelet/cri/streaming"
)

const (
	runtimeName = "porto"
	pauseImage  = "k8s.gcr.io/pause:3.7"
	// loopback + default
	networkAttachCount     = 2
	ifPrefixName           = "veth"
	defaultIfName          = "veth0"
	maxSymlinkResolveDepth = 10
	defaultEnvPath         = "/usr/local/bin:/usr/local/sbin:/usr/bin:/usr/sbin:/bin:/sbin"
)

var excludedMountSources = []string{"/dev", "/sys"}

type PortodshimRuntimeMapper struct {
	containerStateMap map[string]v1.ContainerState
	podStateMap       map[string]v1.PodSandboxState
	netPlugin         cni.CNI
	streamingServer   streaming.Server
}

func NewPortodshimRuntimeMapper() (*PortodshimRuntimeMapper, error) {
	rm := &PortodshimRuntimeMapper{}

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
		rm.netPlugin = netPlugin
	}

	rm.containerStateMap = map[string]v1.ContainerState{
		"stopped":    v1.ContainerState_CONTAINER_CREATED,
		"paused":     v1.ContainerState_CONTAINER_RUNNING,
		"starting":   v1.ContainerState_CONTAINER_RUNNING,
		"running":    v1.ContainerState_CONTAINER_RUNNING,
		"stopping":   v1.ContainerState_CONTAINER_RUNNING,
		"respawning": v1.ContainerState_CONTAINER_RUNNING,
		"meta":       v1.ContainerState_CONTAINER_RUNNING,
		"dead":       v1.ContainerState_CONTAINER_EXITED,
	}

	rm.podStateMap = map[string]v1.PodSandboxState{
		"stopped":    v1.PodSandboxState_SANDBOX_NOTREADY,
		"paused":     v1.PodSandboxState_SANDBOX_NOTREADY,
		"starting":   v1.PodSandboxState_SANDBOX_NOTREADY,
		"running":    v1.PodSandboxState_SANDBOX_READY,
		"stopping":   v1.PodSandboxState_SANDBOX_NOTREADY,
		"respawning": v1.PodSandboxState_SANDBOX_NOTREADY,
		"meta":       v1.PodSandboxState_SANDBOX_NOTREADY,
		"dead":       v1.PodSandboxState_SANDBOX_NOTREADY,
	}

	rm.streamingServer, err = NewStreamingServer(fmt.Sprintf("%s:%s", StreamingServerAddress, StreamingServerPort))
	if err != nil {
		zap.S().Warnf("failed to create streaming server: %v", err)
	}
	go func() {
		err = rm.streamingServer.Start(true)
		if err != nil {
			zap.S().Warnf("failed to start streaming server: %v", err)
		}
	}()

	return rm, nil
}

// INTERNAL
func (m *PortodshimRuntimeMapper) getContainerState(ctx context.Context, id string) v1.ContainerState {
	pc := getPortoClient(ctx)

	state, err := pc.GetProperty(id, "state")
	if err != nil {
		return v1.ContainerState_CONTAINER_UNKNOWN
	}

	return m.containerStateMap[state]
}

func (m *PortodshimRuntimeMapper) getPodState(ctx context.Context, id string) v1.PodSandboxState {
	pc := getPortoClient(ctx)

	state, err := pc.GetProperty(id, "state")
	if err != nil {
		return v1.PodSandboxState_SANDBOX_NOTREADY
	}

	return m.podStateMap[state]
}

func createId(name string) string {
	length := 58
	if len(name) < length {
		length = len(name)
	}
	// max length of return value is 58 + 1 + 4 = 63, so container id <= 127
	return fmt.Sprintf("%s-%04x", name[:length], rand.Intn(65536))
}

func (m *PortodshimRuntimeMapper) prepareContainerNetwork(ctx context.Context, id string, cfg *v1.PodSandboxConfig) error {
	pc := getPortoClient(ctx)

	nsOpts := cfg.GetLinux().GetSecurityContext().GetNamespaceOptions()
	if nsOpts.Network == v1.NamespaceMode_NODE {
		return nil
	}

	if m.netPlugin == nil {
		return fmt.Errorf("cni wasn't initialized")
	}

	netnsPath, err := netns.NewNetNS(NetnsDir)
	if err != nil {
		return fmt.Errorf("failed to create network namespace, pod %s: %w", id, err)
	}
	cniNSOpts := []cni.NamespaceOpts{
		cni.WithCapability(cri.PodAnnotations, cfg.Annotations),
	}
	result, err := m.netPlugin.Setup(ctx, id, netnsPath.GetPath(), cniNSOpts...)
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
	if err = pc.SetProperty(id, "hostname", cfg.GetHostname()); err != nil {
		return fmt.Errorf("failed set porto prop hostname, pod %s: %w", id, err)
	}

	// net
	err = pc.SetProperty(id, "net", fmt.Sprintf("netns %s", filepath.Base(netnsPath.GetPath())))
	if err != nil {
		return fmt.Errorf("failed set porto prop net, pod %s: %w", id, err)
	}
	err = pc.SetProperty(id, "ip", strings.Join(addrs, ";"))
	if err != nil {
		return fmt.Errorf("failed set porto prop ip, pod %s: %w", id, err)
	}

	// sysctl
	sysctls := []string{}
	for k, v := range cfg.GetLinux().GetSysctls() {
		sysctls = append(sysctls, fmt.Sprintf("%s:%s", k, v))
	}
	err = pc.SetProperty(id, "sysctl", strings.Join(sysctls, ";"))
	if err != nil {
		return fmt.Errorf("failed set porto prop sysctl, pod %s: %w", id, err)
	}

	for k, v := range cfg.GetAnnotations() {
		if k == filepath.Join(KubeResourceDomain, "net-tx") {
			err = pc.SetProperty(id, "net_limit", fmt.Sprintf("%s: %s", ifPrefixName, v))
			if err != nil {
				return fmt.Errorf("failed set porto prop net_limit, pod %s: %w", id, err)
			}
		} else if k == filepath.Join(KubeResourceDomain, "net-rx") {
			err = pc.SetProperty(id, "net_rx_limit", fmt.Sprintf("%s: %s", ifPrefixName, v))
			if err != nil {
				return fmt.Errorf("failed set porto prop net_rx_limit, pod %s: %w", id, err)
			}
		}
	}

	return nil
}

func prepareContainerResources(ctx context.Context, id string, cfg *v1.LinuxContainerResources) error {
	pc := getPortoClient(ctx)

	if cfg == nil {
		return nil
	}

	// cpu
	if err := pc.SetProperty(id, "cpu_limit", fmt.Sprintf("%fc", float64(cfg.CpuQuota)/100000)); err != nil {
		return fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	if err := pc.SetProperty(id, "cpu_guarantee", fmt.Sprintf("%fc", float64(cfg.CpuQuota)/100000)); err != nil {
		return fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// memory
	if err := pc.SetProperty(id, "memory_limit", strconv.FormatInt(cfg.MemoryLimitInBytes, 10)); err != nil {
		return fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	if err := pc.SetProperty(id, "memory_guarantee", strconv.FormatInt(cfg.MemoryLimitInBytes, 10)); err != nil {
		return fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	return nil
}

func wrapCmdWithLogShim(cmd []string) []string {
	// No logs needed for pause command
	if cmd[0] != "/pause" {
		cmd = append([]string{"/usr/sbin/logshim"}, cmd...)
	}
	return cmd
}

func isChrootPathExecutable(root, path string, depth int) (bool, error) {
	if depth > maxSymlinkResolveDepth {
		return false, fmt.Errorf("too many levels of symbolic links %d while maximum is %d", depth, maxSymlinkResolveDepth)
	}
	absPath := filepath.Join(root, path)
	fi, err := os.Lstat(absPath)
	if err != nil {
		return false, fmt.Errorf("failed to lstat path '%s' inside root '%s': %w", path, root, err)
	}
	if fi.Mode()&os.ModeSymlink > 0 {
		target, err := os.Readlink(absPath)
		if err != nil {
			return false, fmt.Errorf("failed to read link '%s' inside root '%s': %w", path, root, err)
		}
		if len(target) > 0 && target[0] != '/' {
			target = filepath.Join(filepath.Dir(path), target)
		}
		return isChrootPathExecutable(root, target, depth+1)
	}
	return fi.Mode()&0o100 > 0 && !fi.IsDir(), nil
}

func findChrootExecutable(ctx context.Context, path, root, cmd string) string {
	var binaryPath string
	paths := strings.Split(path, ":")
	for _, p := range paths {
		candidate := filepath.Join(p, cmd)
		ok, err := isChrootPathExecutable(root, candidate, 1)
		DebugLog(ctx, "checking candidate path '%s' for cmd '%s' in root '%s', ok: %t, err: %v", candidate, cmd, root, ok, err)
		if ok {
			binaryPath = candidate
			break
		}
	}
	return binaryPath
}

func findEnvPathOrDefault(env []*rpc.TContainerEnvVar) string {
	for _, e := range env {
		if *e.Name == "PATH" {
			return *e.Value
		}
	}
	return defaultEnvPath
}

func prepareContainerCommand(ctx context.Context, id string, cfgCmd, cfgArgs, imgCmd []string, env []*rpc.TContainerEnvVar, disableLogShim bool) error {
	pc := getPortoClient(ctx)

	cmd := imgCmd
	if len(cfgCmd) > 0 {
		cmd = cfgCmd
	}
	cmd = append(cmd, cfgArgs...)

	// Wrap non-absolute path command into call to /bin/sh -c
	if len(cmd) < 1 {
		return fmt.Errorf("got empty command for container %s", id)
	}
	if len(cmd[0]) < 1 {
		return fmt.Errorf("got malformed command '%v' for container %s", cmd, id)
	}
	// Try to find out binary path inside chroot if we have non-absolute command
	if cmd[0][0] != '/' {
		rootPath, err := pc.GetProperty(id, "root_path")
		if err != nil {
			return fmt.Errorf("failed to get root_path for container %s: %w", id, err)
		}
		execPath := findChrootExecutable(ctx, findEnvPathOrDefault(env), rootPath, cmd[0])
		if execPath != "" {
			cmd[0] = execPath
		} else {
			// Last resort, we failed to find out command, so let sh decide.
			cmd = append([]string{"/bin/sh", "-c"}, strings.Join(cmd, " "))
		}
	}

	if !disableLogShim {
		cmd = wrapCmdWithLogShim(cmd)
	}

	req := &rpc.TContainerSpec{
		Name: &id,
		CommandArgv: &rpc.TContainerCommandArgv{
			Argv: cmd,
		},
	}
	return pc.UpdateFromSpec(req)
}

func envToVars(env []string) []*rpc.TContainerEnvVar {
	var envVars []*rpc.TContainerEnvVar

	for _, i := range env {
		keyValue := strings.SplitN(i, "=", 2)
		envVars = append(envVars, &rpc.TContainerEnvVar{
			Name:  &keyValue[0],
			Value: &keyValue[1],
		})
	}

	return envVars
}

func prepareContainerEnv(ctx context.Context, id string, env []*v1.KeyValue, image *rpc.TDockerImage) ([]*rpc.TContainerEnvVar, error) {
	pc := getPortoClient(ctx)

	envVars := envToVars(image.GetConfig().GetEnv())

	for _, i := range env {
		envVars = append(envVars, &rpc.TContainerEnvVar{
			Name:  &i.Key,
			Value: &i.Value,
		})
	}

	req := &rpc.TContainerSpec{
		Name: &id,
		Env: &rpc.TContainerEnv{
			Var: envVars,
		},
	}

	return envVars, pc.UpdateFromSpec(req)
}

func prepareContainerRoot(ctx context.Context, id string, rootPath string, image string) (string, error) {
	pc := getPortoClient(ctx)

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
	_, err = pc.CreateVolume(rootAbsPath, map[string]string{
		"containers": id,
		"image":      image,
		"backend":    "overlay",
	})
	if err != nil {
		_ = os.RemoveAll(rootAbsPath)
		return rootAbsPath, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	if err = pc.SetProperty(id, "root", rootPath); err != nil {
		_ = os.RemoveAll(rootAbsPath)
		return rootAbsPath, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	return rootAbsPath, nil
}

func prepareContainerMount(ctx context.Context, id string, mount *v1.Mount) error {
	pc := getPortoClient(ctx)

	volume, err := pc.CreateVolume("", map[string]string{
		"backend": "bind",
		"storage": mount.HostPath,
	})
	if err != nil && err.(*porto.Error).Code != rpc.EError_VolumeAlreadyExists {
		return fmt.Errorf("%s: %s %s %v", getCurrentFuncName(), id, mount.HostPath, err)
	}
	defer func() {
		err := pc.UnlinkVolume(volume.Path, "/", "")
		if err != nil && err.(*porto.Error).Code != rpc.EError_VolumeNotLinked {
			zap.S().Errorf("failed to unlink volume %s from root container: %v", volume.Path, err)
		}
	}()
	err = pc.LinkVolume(volume.Path, id, mount.ContainerPath, false, mount.Readonly)
	if err != nil {
		return fmt.Errorf("%s: %s %s %s %v", getCurrentFuncName(), id, mount.HostPath, mount.ContainerPath, err)
	}
	return nil
}

func sliceContainsString(s []string, str string) bool {
	for _, v := range s {
		if v == str {
			return true
		}
	}

	return false
}

func prepareContainerMounts(ctx context.Context, id string, mounts []*v1.Mount) error {
	// Mount logshim binary to container
	mounts = append(mounts,
		&v1.Mount{
			ContainerPath: "/usr/sbin/logshim",
			HostPath:      "/usr/sbin/logshim",
			Readonly:      true,
			Propagation:   v1.MountPropagation_PROPAGATION_PRIVATE,
		})

	for _, mount := range mounts {
		// pre-normalize volume path for porto as it expects "normal" path
		mount.ContainerPath = filepath.Clean(mount.ContainerPath)
		mount.HostPath = filepath.Clean(mount.HostPath)
		if sliceContainsString(excludedMountSources, mount.HostPath) {
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
		err := prepareContainerMount(ctx, id, mount)
		if err != nil {
			return err
		}
	}
	return nil
}

func prepareContainerResolvConf(ctx context.Context, id string, cfg *v1.DNSConfig) error {
	pc := getPortoClient(ctx)

	if cfg != nil {
		resolvConf := []string{}
		for _, i := range cfg.GetServers() {
			resolvConf = append(resolvConf, fmt.Sprintf("%s %s", "nameserver", i))
		}
		resolvConf = append(resolvConf, fmt.Sprintf("%s %s", "search", strings.Join(cfg.GetSearches(), " ")))
		resolvConf = append(resolvConf, fmt.Sprintf("%s %s\n", "options", strings.Join(cfg.GetOptions(), " ")))
		return pc.SetProperty(id, "resolv_conf", strings.Join(resolvConf, ";"))
	}

	return nil
}

func getContainerNetNS(ctx context.Context, id string) (string, error) {
	pc := getPortoClient(ctx)

	netProp, err := pc.GetProperty(id, "net")
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
	pc := getPortoClient(ctx)

	labelsString := ""
	for label, value := range labels {
		labelsString += fmt.Sprintf("%s:%s;", convertLabel(label, true, prefix), convertLabel(value, true, ""))
	}

	return pc.SetProperty(id, "labels", labelsString)
}

func getLabels(ctx context.Context, id string, prefix string) map[string]string {
	pc := getPortoClient(ctx)

	labels, err := pc.GetProperty(id, "labels")
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
	pc := getPortoClient(ctx)

	valueString, err := pc.GetProperty(id, property)
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
	pc := getPortoClient(ctx)

	valueString, err := pc.GetProperty(id, property)
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
	pc := getPortoClient(ctx)

	value, err := pc.GetProperty(id, property)
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
	pc := getPortoClient(ctx)

	cpu := getUintProperty(ctx, id, "cpu_usage")
	timestamp := time.Now().UnixNano()

	response, err := pc.List1(id + "/***")
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
	pc := getPortoClient(ctx)

	if !isContainer(id) {
		return ""
	}

	imageDescriptions, err := pc.ListVolumes(VolumesDir+"/"+id, id)
	if err != nil {
		zap.S().Warnf("%s: %v", getCurrentFuncName(), err)
		return ""
	}
	return imageDescriptions[0].Properties["image"]
}

func getPodSandboxNetworkStatus(ctx context.Context, id string) *v1.PodSandboxNetworkStatus {
	pc := getPortoClient(ctx)

	addresses, err := pc.GetProperty(id, "ip")
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

func parsePropertyNetNS(prop string) string {
	netNSL := strings.Fields(prop)
	if netNSL[0] == "netns" && len(netNSL) > 1 {
		return netNSL[1]
	}
	return ""
}

// RUNTIME SERVICE INTERFACE
func (m *PortodshimRuntimeMapper) Version(ctx context.Context, req *v1.VersionRequest) (*v1.VersionResponse, error) {
	pc := getPortoClient(ctx)

	tag, _, err := pc.GetVersion()
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

func (m *PortodshimRuntimeMapper) RunPodSandbox(ctx context.Context, req *v1.RunPodSandboxRequest) (*v1.RunPodSandboxResponse, error) {
	pc := getPortoClient(ctx)

	id := createId(req.GetConfig().GetMetadata().GetName())
	err := pc.Create(id)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// get image
	image, err := pc.DockerImageStatus(pauseImage, "")
	if err != nil {
		_ = pc.Destroy(id)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// resources
	res := req.GetConfig().GetLinux().GetResources()
	if res != nil {
		if err = prepareContainerResources(ctx, id, res); err != nil {
			_ = pc.Destroy(id)
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
		_ = pc.Destroy(id)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	if err = setLabels(ctx, id, req.GetConfig().GetAnnotations(), "ANNOTATION"); err != nil {
		_ = pc.Destroy(id)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// root
	rootPath, err := prepareContainerRoot(ctx, id, "", pauseImage)
	if err != nil {
		_ = pc.Destroy(id)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// Container prepare order is mandatory!
	// env and command MUST be prepared ONLY AFTER container chroot ready
	env, err := prepareContainerEnv(ctx, id, []*v1.KeyValue{}, image)
	if err != nil {
		_ = pc.Destroy(id)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// command + args
	if err = prepareContainerCommand(ctx, id, []string{}, []string{}, image.GetConfig().GetCmd(), env, false); err != nil {
		_ = pc.Destroy(id)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// network
	cfg := req.GetConfig()
	if cfg != nil && cfg.GetLinux() != nil {
		if err = m.prepareContainerNetwork(ctx, id, cfg); err != nil {
			_ = pc.Destroy(id)
			_ = os.RemoveAll(rootPath)
			return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
		}
	}

	// pod starting
	err = pc.Start(id)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	return &v1.RunPodSandboxResponse{
		PodSandboxId: id,
	}, nil
}

func (m *PortodshimRuntimeMapper) StopPodSandbox(ctx context.Context, req *v1.StopPodSandboxRequest) (*v1.StopPodSandboxResponse, error) {
	pc := getPortoClient(ctx)

	id := req.GetPodSandboxId()

	if state := getStringProperty(ctx, id, "state"); state == "running" {
		err := pc.Kill(id, 15)
		if err != nil {
			return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
		}
	}

	return &v1.StopPodSandboxResponse{}, nil
}

func (m *PortodshimRuntimeMapper) RemovePodSandbox(ctx context.Context, req *v1.RemovePodSandboxRequest) (*v1.RemovePodSandboxResponse, error) {
	pc := getPortoClient(ctx)

	id := req.GetPodSandboxId()
	netProp, err := pc.GetProperty(id, "net")
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	if err = pc.Destroy(id); err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	rootPath := VolumesDir + "/" + id
	if err = os.RemoveAll(rootPath); err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// removes the network from the pod
	netnsProp := parsePropertyNetNS(netProp)
	if netnsProp != "" {
		netnsPath := netns.LoadNetNS(filepath.Join(NetnsDir, netnsProp))
		if err = m.netPlugin.Remove(ctx, id, netnsPath.GetPath(), cni.WithLabels(map[string]string{})); err != nil {
			return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
		}
		if err = netnsPath.Remove(); err != nil {
			return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
		}
	}
	return &v1.RemovePodSandboxResponse{}, nil
}

func (m *PortodshimRuntimeMapper) PodSandboxStatus(ctx context.Context, req *v1.PodSandboxStatusRequest) (*v1.PodSandboxStatusResponse, error) {
	id := req.GetPodSandboxId()

	netNSMode, err := getContainerNetNSMode(ctx, id)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	resp := &v1.PodSandboxStatusResponse{
		Status: &v1.PodSandboxStatus{
			Id:        id,
			Metadata:  getPodMetadata(ctx, id),
			State:     m.getPodState(ctx, id),
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

	return resp, nil
}

func (m *PortodshimRuntimeMapper) PodSandboxStats(ctx context.Context, req *v1.PodSandboxStatsRequest) (*v1.PodSandboxStatsResponse, error) {
	id := req.GetPodSandboxId()

	return &v1.PodSandboxStatsResponse{
		Stats: getPodStats(ctx, id),
	}, nil
}

func (m *PortodshimRuntimeMapper) ListPodSandbox(ctx context.Context, req *v1.ListPodSandboxRequest) (*v1.ListPodSandboxResponse, error) {
	pc := getPortoClient(ctx)

	targetId := req.GetFilter().GetId()
	targetState := req.GetFilter().GetState()
	targetLabels := req.GetFilter().GetLabelSelector()

	mask := "*"
	if targetId != "" {
		mask = targetId
	}

	response, err := pc.List1(mask)
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
		state := m.getPodState(ctx, id)
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

func (m *PortodshimRuntimeMapper) CreateContainer(ctx context.Context, req *v1.CreateContainerRequest) (*v1.CreateContainerResponse, error) {
	pc := getPortoClient(ctx)

	// <id> := <podId>/<containerId>
	podId := req.GetPodSandboxId()
	containerId := createId(req.GetConfig().GetMetadata().GetName())
	id := filepath.Join(podId, containerId)
	err := pc.Create(id)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// get image
	// TODO: define imageName in rpc.TDockerImage
	imageName := req.GetConfig().GetImage().GetImage()
	image, err := pc.DockerImageStatus(imageName, "")
	if err != nil {
		_ = pc.Destroy(id)
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
		_ = pc.Destroy(id)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)

	}
	if err = setLabels(ctx, id, req.GetConfig().GetAnnotations(), "ANNOTATION"); err != nil {
		_ = pc.Destroy(id)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// resolv.conf
	if err = prepareContainerResolvConf(ctx, id, req.GetSandboxConfig().GetDnsConfig()); err != nil {
		_ = pc.Destroy(id)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// root
	rootPath, err := prepareContainerRoot(ctx, id, "/"+containerId, imageName)
	if err != nil {
		_ = pc.Destroy(id)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// mounts
	if err = prepareContainerMounts(ctx, id, req.GetConfig().GetMounts()); err != nil {
		_ = pc.Destroy(id)
		_ = os.RemoveAll(rootPath)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// Container prepare order is mandatory!
	// env and command MUST be prepared ONLY AFTER container chroot ready
	env, err := prepareContainerEnv(ctx, id, req.GetConfig().GetEnvs(), image)
	if err != nil {
		_ = pc.Destroy(id)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// command + args
	if err = prepareContainerCommand(ctx, id, req.GetConfig().GetCommand(), req.GetConfig().GetArgs(), image.GetConfig().GetCmd(), env, false); err != nil {
		_ = pc.Destroy(id)
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	return &v1.CreateContainerResponse{
		ContainerId: id,
	}, nil
}

func (m *PortodshimRuntimeMapper) StartContainer(ctx context.Context, req *v1.StartContainerRequest) (*v1.StartContainerResponse, error) {
	pc := getPortoClient(ctx)

	id := req.GetContainerId()
	if !isContainer(id) {
		return nil, fmt.Errorf("%s: %s specified ID belongs to pod", getCurrentFuncName(), id)
	}

	err := pc.Start(id)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	return &v1.StartContainerResponse{}, nil
}

func (m *PortodshimRuntimeMapper) StopContainer(ctx context.Context, req *v1.StopContainerRequest) (*v1.StopContainerResponse, error) {
	pc := getPortoClient(ctx)

	id := req.GetContainerId()
	if !isContainer(id) {
		return nil, fmt.Errorf("%s: %s specified ID belongs to pod", getCurrentFuncName(), id)
	}

	if state := getStringProperty(ctx, id, "state"); state == "running" {
		err := pc.Kill(id, 15)
		if err != nil {
			return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
		}
	}

	return &v1.StopContainerResponse{}, nil
}

func (m *PortodshimRuntimeMapper) RemoveContainer(ctx context.Context, req *v1.RemoveContainerRequest) (*v1.RemoveContainerResponse, error) {
	pc := getPortoClient(ctx)

	id := req.GetContainerId()
	if !isContainer(id) {
		return nil, fmt.Errorf("%s: %s specified ID belongs to pod", getCurrentFuncName(), id)
	}

	err := pc.Destroy(id)
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

func (m *PortodshimRuntimeMapper) ListContainers(ctx context.Context, req *v1.ListContainersRequest) (*v1.ListContainersResponse, error) {
	pc := getPortoClient(ctx)
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

	response, err := pc.List1(mask)
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
		state := m.getContainerState(ctx, id)
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
			State:       m.getContainerState(ctx, id),
			CreatedAt:   getTimeProperty(ctx, id, "creation_time[raw]"),
			Labels:      labels,
			Annotations: getLabels(ctx, id, "ANNOTATION"),
		})
	}

	return &v1.ListContainersResponse{
		Containers: containers,
	}, nil
}

func (m *PortodshimRuntimeMapper) ContainerStatus(ctx context.Context, req *v1.ContainerStatusRequest) (*v1.ContainerStatusResponse, error) {
	id := req.GetContainerId()
	if !isContainer(id) {
		return nil, fmt.Errorf("%s: specified ID belongs to pod", getCurrentFuncName())
	}

	image := getContainerImage(ctx, id)

	resp := &v1.ContainerStatusResponse{
		Status: &v1.ContainerStatus{
			Id:         id,
			Metadata:   getContainerMetadata(ctx, id),
			State:      m.getContainerState(ctx, id),
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

	return resp, nil
}

func (m *PortodshimRuntimeMapper) UpdateContainerResources(ctx context.Context, req *v1.UpdateContainerResourcesRequest) (*v1.UpdateContainerResourcesResponse, error) {
	return nil, fmt.Errorf("not implemented UpdateContainerResources")
}

func (m *PortodshimRuntimeMapper) ReopenContainerLog(ctx context.Context, req *v1.ReopenContainerLogRequest) (*v1.ReopenContainerLogResponse, error) {
	// TODO: реализовать ReopenContainerLog
	return &v1.ReopenContainerLogResponse{}, nil
}

func (m *PortodshimRuntimeMapper) ExecSync(ctx context.Context, req *v1.ExecSyncRequest) (*v1.ExecSyncResponse, error) {
	pc := getPortoClient(ctx)
	execContainerID := req.GetContainerId() + "/" + createId("exec-sync")
	if err := pc.Create(execContainerID); err != nil {
		return nil, fmt.Errorf("failed to create exec container %s: %w", execContainerID, err)
	}
	defer pc.Destroy(execContainerID)

	env, err := pc.GetProperty(req.GetContainerId(), "env")
	if err != nil {
		return nil, fmt.Errorf("failed to get parent container %s env prop: %w", req.GetContainerId(), err)
	}
	if err := pc.SetProperty(execContainerID, "env", env); err != nil {
		return nil, fmt.Errorf("failed to set env for exec container %s: %w", execContainerID, err)
	}
	if err := prepareContainerCommand(ctx, execContainerID, req.GetCmd(), nil, nil, envToVars(strings.Split(env, ";")), true); err != nil {
		return nil, fmt.Errorf("failed to prepare command '%v' for exec container %s: %w", req.Cmd, execContainerID, err)
	}

	if err := pc.Start(execContainerID); err != nil {
		return nil, fmt.Errorf("failed to start exec container %s command '%s': %w", execContainerID, strings.Join(req.Cmd, " "), err)
	}
	if _, err := pc.Wait([]string{execContainerID}, time.Duration(req.Timeout)*time.Second); err != nil {
		return nil, fmt.Errorf("failed to wait exec container %s exit: %w", execContainerID, err)
	}

	exit_code, err := pc.GetProperty(execContainerID, "exit_code")
	if err != nil {
		return nil, fmt.Errorf("failed to get container %s exit_code: %w", execContainerID, err)
	}
	code, err := strconv.ParseInt(exit_code, 10, 32)
	if err != nil {
		return nil, fmt.Errorf("failed to parse exit_code '%s': %w", exit_code, err)
	}

	// TODO: maybe read whole stdout/stderr file not just tail from porto?
	streams, err := pc.Get([]string{execContainerID}, []string{"stdout", "stderr"})
	if err != nil {
		return nil, fmt.Errorf("failed to get container %s stdout and stderr: %w", execContainerID, err)
	}
	rsp := &v1.ExecSyncResponse{
		Stdout:   []byte(streams[execContainerID]["stdout"].Value),
		Stderr:   []byte(streams[execContainerID]["stderr"].Value),
		ExitCode: int32(code),
	}
	return rsp, nil
}

func (m *PortodshimRuntimeMapper) Exec(ctx context.Context, req *v1.ExecRequest) (*v1.ExecResponse, error) {
	resp, err := m.streamingServer.GetExec(req)
	if err != nil {
		return nil, fmt.Errorf("unable to prepare exec endpoint: %v", err)
	}

	return resp, nil
}

func (m *PortodshimRuntimeMapper) Attach(ctx context.Context, req *v1.AttachRequest) (*v1.AttachResponse, error) {
	return nil, fmt.Errorf("not implemented Attach")
}

func (m *PortodshimRuntimeMapper) PortForward(ctx context.Context, req *v1.PortForwardRequest) (*v1.PortForwardResponse, error) {
	return nil, fmt.Errorf("not implemented PortForward")
}

func (m *PortodshimRuntimeMapper) ContainerStats(ctx context.Context, req *v1.ContainerStatsRequest) (*v1.ContainerStatsResponse, error) {
	id := req.GetContainerId()
	if !isContainer(id) {
		return nil, fmt.Errorf("%s: specified ID belongs to pod", getCurrentFuncName())
	}

	return &v1.ContainerStatsResponse{
		Stats: getContainerStats(ctx, id),
	}, nil
}

func (m *PortodshimRuntimeMapper) ListContainerStats(ctx context.Context, req *v1.ListContainerStatsRequest) (*v1.ListContainerStatsResponse, error) {
	pc := getPortoClient(ctx)

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

	response, err := pc.List1(mask)
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

func (m *PortodshimRuntimeMapper) ListPodSandboxStats(ctx context.Context, req *v1.ListPodSandboxStatsRequest) (*v1.ListPodSandboxStatsResponse, error) {
	pc := getPortoClient(ctx)

	targetId := req.GetFilter().GetId()
	targetLabels := req.GetFilter().GetLabelSelector()

	mask := "*"
	if targetId != "" {
		mask = targetId
	}

	response, err := pc.List1(mask)
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

func (m *PortodshimRuntimeMapper) UpdateRuntimeConfig(ctx context.Context, req *v1.UpdateRuntimeConfigRequest) (*v1.UpdateRuntimeConfigResponse, error) {
	return nil, fmt.Errorf("not implemented UpdateRuntimeConfig")
}

func (m *PortodshimRuntimeMapper) Status(ctx context.Context, req *v1.StatusRequest) (*v1.StatusResponse, error) {
	pc := getPortoClient(ctx)

	if _, _, err := pc.GetVersion(); err != nil {
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
