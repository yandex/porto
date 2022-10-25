package main

const (
	// sockets
	PortodshimSocket = "/run/portodshim.sock"
	PortoSocket      = "/run/portod.socket"
	// common dirs
	LogsDir    = "/var/log/portodshim"
	ImagesDir  = "/place/porto_docker"
	VolumesDir = "/place/portodshim_volumes"
	// cni dirs
	NetworkPluginConfDir = "/etc/cni/net.d"
	NetworkPluginBinDir  = "/opt/cni/bin"
	NetnsDir             = "/var/run/netns"
	// paths
	PortodshimLogPath = LogsDir + "/portodshim.log"
	// k8s
	KubeResourceDomain = "yandex.net"
)
