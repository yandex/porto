package main

import (
	"context"
	"fmt"
	"strings"
	"syscall"
	"time"

	"github.com/yandex/porto/src/api/go/porto"
	"github.com/yandex/porto/src/api/go/porto/pkg/rpc"
	"go.uber.org/zap"
	v1 "k8s.io/cri-api/pkg/apis/runtime/v1"
)

const (
	ImagesPath = "/place/porto_docker"
)

type PortodshimImageMapper struct{}

// INTERNAL
func parseImageName(name string) (string, string, string) {
	image, digest, _ := strings.Cut(name, "@")
	image, tag, tagFound := strings.Cut(image, ":")
	if !tagFound {
		tag = "latest"
	}

	return image, tag, digest
}
func getImageStruct(fullName string, tags []string) *v1.Image {
	// default value of size, it must be > 0
	size := uint64(4)

	name, _, digest := parseImageName(fullName)

	return &v1.Image{
		Id:          name + "@sha256:" + digest,
		RepoTags:    tags,
		RepoDigests: []string{name + "@sha256:" + digest},
		Size_:       size,
		Uid: &v1.Int64Value{
			Value: 1,
		},
		Username: "",
		Spec: &v1.ImageSpec{
			Image:       name + "@sha256:" + digest,
			Annotations: map[string]string{},
		},
	}
}

// IMAGE SERVICE INTERFACE
func (mapper *PortodshimImageMapper) ListImages(ctx context.Context, req *v1.ListImagesRequest) (*v1.ListImagesResponse, error) {
	zap.S().Debugf("call %s: %s", getCurrentFuncName(), req.GetFilter().GetImage())

	portoClient := ctx.Value("portoClient").(porto.API)

	portoImages, err := portoClient.ListDockerImages("", "")
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// image tags and digests collection
	tagsMap := make(map[string][]string)
	for _, image := range portoImages {
		name, tag, digest := parseImageName(image.GetFullName())
		tagsMap[digest] = append(tagsMap[digest], name+":"+tag)
	}

	// we have tags and digests for every image here
	var images []*v1.Image
	for _, image := range portoImages {
		_, _, digest := parseImageName(image.GetFullName())
		images = append(images, getImageStruct(image.GetFullName(), tagsMap[digest]))
	}

	return &v1.ListImagesResponse{
		Images: images,
	}, nil
}
func (mapper *PortodshimImageMapper) ImageStatus(ctx context.Context, req *v1.ImageStatusRequest) (*v1.ImageStatusResponse, error) {
	zap.S().Debugf("call %s: %s", getCurrentFuncName(), req.GetImage())

	portoClient := ctx.Value("portoClient").(porto.API)

	image, err := portoClient.DockerImageStatus(req.GetImage().GetImage(), "")
	if err != nil {
		if err.(*porto.Error).Errno == rpc.EError_DockerImageNotFound {
			return &v1.ImageStatusResponse{
				Image: nil,
				Info:  map[string]string{},
			}, nil
		}
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// TODO: add tags into TDockerImage and delete this
	name, _, targetDigest := parseImageName(image.GetFullName())
	images, err := portoClient.ListDockerImages("", name+"***")
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// image tags and digests collection
	var tags []string
	for _, image := range images {
		name, tag, digest := parseImageName(image.GetFullName())
		if digest == targetDigest {
			tags = append(tags, name+":"+tag)
		}
	}

	return &v1.ImageStatusResponse{
		Image: getImageStruct(image.GetFullName(), tags),
	}, nil
}
func (mapper *PortodshimImageMapper) PullImage(ctx context.Context, req *v1.PullImageRequest) (*v1.PullImageResponse, error) {
	zap.S().Debugf("call %s: %s %s", getCurrentFuncName(), req.GetImage(), req.GetAuth())

	portoClient := ctx.Value("portoClient").(porto.API)

	image, err := portoClient.PullDockerImage(req.GetImage().GetImage(), "", req.GetAuth().GetIdentityToken(), req.GetAuth().GetServerAddress(), req.GetAuth().GetAuth())
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	return &v1.PullImageResponse{
		ImageRef: image.GetFullName(),
	}, nil
}
func (mapper *PortodshimImageMapper) RemoveImage(ctx context.Context, req *v1.RemoveImageRequest) (*v1.RemoveImageResponse, error) {
	zap.S().Debugf("call %s: %s", getCurrentFuncName(), req.GetImage())

	portoClient := ctx.Value("portoClient").(porto.API)

	err := portoClient.RemoveDockerImage(req.GetImage().GetImage(), "")
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	return &v1.RemoveImageResponse{}, nil
}
func (mapper *PortodshimImageMapper) ImageFsInfo(ctx context.Context, req *v1.ImageFsInfoRequest) (*v1.ImageFsInfoResponse, error) {
	zap.S().Debugf("call %s", getCurrentFuncName())

	stat := syscall.Statfs_t{}
	err := syscall.Statfs(ImagesPath, &stat)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	return &v1.ImageFsInfoResponse{
		ImageFilesystems: []*v1.FilesystemUsage{
			{
				Timestamp: time.Now().UnixNano(),
				FsId: &v1.FilesystemIdentifier{
					Mountpoint: ImagesPath,
				},
				UsedBytes:  &v1.UInt64Value{Value: (stat.Blocks - stat.Bfree) * uint64(stat.Bsize)},
				InodesUsed: &v1.UInt64Value{Value: stat.Files - stat.Ffree},
			},
		},
	}, nil
}
