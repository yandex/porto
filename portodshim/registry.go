package main

import (
	"os"
	"strings"
)

type RegistryInfo struct {
	Host        string
	AuthToken   string
	AuthPath    string
	AuthService string
}

const (
	DefaultDockerRegistry = "registry-1.docker.io"
)

var (
	KnownRegistries = map[string]RegistryInfo{
		DefaultDockerRegistry: RegistryInfo{
			Host: DefaultDockerRegistry,
		},
		"registry.yandex.net": RegistryInfo{
			Host:      "registry.yandex.net",
			AuthToken: "file:/var/run/portodshim/auth_tokens/registry.yandex.net",
		},
		"quay.io": RegistryInfo{
			Host:     "quay.io",
			AuthPath: "https://quay.io/v2/auth",
		},
	}
)

func InitKnownRegistries() error {
	for host, registry := range KnownRegistries {
		if strings.HasPrefix(registry.AuthToken, "file:") {
			content, err := os.ReadFile(registry.AuthToken[5:])
			if err != nil {
				return err
			}
			registry.AuthToken = strings.TrimSpace(string(content))
			KnownRegistries[host] = registry
		}
	}
	return nil
}

func GetImageRegistry(name string) RegistryInfo {
	host := DefaultDockerRegistry

	slashPos := strings.Index(name, "/")
	if slashPos > -1 {
		host = name[:slashPos]
	}

	if registry, ok := KnownRegistries[host]; ok {
		return registry
	}

	return RegistryInfo{}
}
