package main

import (
	"context"
	"fmt"
	"strings"
	"syscall"
	"time"

	"go.uber.org/zap"
	v1 "k8s.io/cri-api/pkg/apis/runtime/v1"
)

const (
	LayersPath = "/place/porto_layers"
)

type PortodshimImageMapper struct{}

// INTERNAL
func (mapper *PortodshimImageMapper) getImageAndTag(id string) (string, string) {
	splitedImage := strings.Split(id, ":")
	image := splitedImage[0]
	tag := "latest"
	if len(splitedImage) > 1 {
		tag = splitedImage[1]
	}

	return image, tag
}
func (mapper *PortodshimImageMapper) getImageStruct(id string, tags []string, owner string) *v1.Image {
	// default value of size, it must be > 0
	size := uint64(4)

	return &v1.Image{
		Id:          id,
		RepoTags:    tags,
		RepoDigests: []string{},
		Size_:       size,
		Uid: &v1.Int64Value{
			Value: 1,
		},
		Username: owner,
		Spec:     nil,
	}
}

// IMAGE SERVICE INTERFACE
func (mapper *PortodshimImageMapper) ListImages(ctx context.Context, req *v1.ListImagesRequest) (*v1.ListImagesResponse, error) {
	zap.S().Debugf("call %s", getCurrentFuncName())

	portoClient := ctx.Value("portoClient").(API)

	response, err := portoClient.ListLayers2("", "")
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	// image tags computation
	tagsMap := make(map[string][]string)
	imageOwnerMap := make(map[string]string)
	for _, desc := range response {
		image, tag := mapper.getImageAndTag(desc.Name)
		tagsMap[desc.Name] = append(tagsMap[desc.Name], image+":"+tag)
		imageOwnerMap[image] = desc.OwnerUser
	}

	// we have tags for every image here
	var images []*v1.Image
	for _, desc := range response {
		image, _ := mapper.getImageAndTag(desc.Name)
		images = append(images, mapper.getImageStruct(desc.Name, tagsMap[desc.Name], imageOwnerMap[image]))
	}

	return &v1.ListImagesResponse{
		Images: images,
	}, nil
}
func (mapper *PortodshimImageMapper) ImageStatus(ctx context.Context, req *v1.ImageStatusRequest) (*v1.ImageStatusResponse, error) {
	zap.S().Debugf("call %s: %s", getCurrentFuncName(), req.Image.GetImage())

	portoClient := ctx.Value("portoClient").(API)

	// TODO: Убрать заглушку
	rawImage := req.GetImage().GetImage()
	if strings.HasPrefix(rawImage, "k8s.gcr.io/") {
		rawImage = "ubuntu:xenial"
	}
	image, tag := mapper.getImageAndTag(rawImage)

	response, err := portoClient.ListLayers2("", image+"***")
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	if len(response) == 0 {
		return nil, fmt.Errorf("%s: layer not found", getCurrentFuncName())
	}

	var tags []string
	imageFound := false
	for _, desc := range response {
		_, currentTag := mapper.getImageAndTag(desc.Name)
		if currentTag == tag {
			imageFound = true
		}
		tags = append(tags, image+":"+currentTag)
	}

	if !imageFound {
		return nil, fmt.Errorf("%s: layer not found", getCurrentFuncName())
	}

	return &v1.ImageStatusResponse{
		Image: mapper.getImageStruct(image+":"+tag, tags, response[0].OwnerUser),
	}, nil
}
func (mapper *PortodshimImageMapper) PullImage(ctx context.Context, req *v1.PullImageRequest) (*v1.PullImageResponse, error) {
	zap.S().Debugf("call %s: %s", getCurrentFuncName(), req.Image.GetImage())

	// TODO: Убрать заглушку
	return &v1.PullImageResponse{
		ImageRef: req.Image.GetImage(),
	}, nil
}
func (mapper *PortodshimImageMapper) RemoveImage(ctx context.Context, req *v1.RemoveImageRequest) (*v1.RemoveImageResponse, error) {
	zap.S().Debugf("call %s: %s", getCurrentFuncName(), req.Image.GetImage())

	// temporarily
	//portoClient := ctx.Value("portoClient").(API)
	//err := portoClient.RemoveLayer(req.Image.GetImage())
	//if err != nil {
	//	return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	//}

	return &v1.RemoveImageResponse{}, nil
}
func (mapper *PortodshimImageMapper) ImageFsInfo(ctx context.Context, req *v1.ImageFsInfoRequest) (*v1.ImageFsInfoResponse, error) {
	zap.S().Debugf("call %s", getCurrentFuncName())

	stat := syscall.Statfs_t{}
	err := syscall.Statfs(LayersPath, &stat)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	return &v1.ImageFsInfoResponse{
		ImageFilesystems: []*v1.FilesystemUsage{
			{
				Timestamp: time.Now().UnixNano(),
				FsId: &v1.FilesystemIdentifier{
					Mountpoint: LayersPath,
				},
				UsedBytes:  &v1.UInt64Value{Value: (stat.Blocks - stat.Bfree) * uint64(stat.Bsize)},
				InodesUsed: &v1.UInt64Value{Value: stat.Files - stat.Ffree},
			},
		},
	}, nil
}
