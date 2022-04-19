package main

import (
	"context"
	"fmt"
	"strings"
	"syscall"
	"time"

	v1alpha2 "k8s.io/cri-api/pkg/apis/runtime/v1alpha2"
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
func (mapper *PortodshimImageMapper) getImageStruct(id string, tags []string, owner string) *v1alpha2.Image {
	// default value of size, it must be > 0
	size := uint64(4)

	return &v1alpha2.Image{
		Id:          id,
		RepoTags:    tags,
		RepoDigests: []string{},
		Size_:       size,
		Uid: &v1alpha2.Int64Value{
			Value: 1,
		},
		Username: owner,
		Spec:     nil,
	}
}

// IMAGE SERVICE INTERFACE
func (mapper *PortodshimImageMapper) ListImages(ctx context.Context, req *v1alpha2.ListImagesRequest) (*v1alpha2.ListImagesResponse, error) {
	response, err := ctx.Value("portoClient").(API).ListLayers2("", "")
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
	var images []*v1alpha2.Image
	for _, desc := range response {
		image, _ := mapper.getImageAndTag(desc.Name)
		images = append(images, mapper.getImageStruct(desc.Name, tagsMap[desc.Name], imageOwnerMap[image]))
	}

	return &v1alpha2.ListImagesResponse{
		Images: images,
	}, nil
}
func (mapper *PortodshimImageMapper) ImageStatus(ctx context.Context, req *v1alpha2.ImageStatusRequest) (*v1alpha2.ImageStatusResponse, error) {
	// TODO: Решить вопрос с образом pause
	image, tag := mapper.getImageAndTag(req.Image.GetImage())

	response, err := ctx.Value("portoClient").(API).ListLayers2("", image+"***")
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

	return &v1alpha2.ImageStatusResponse{
		Image: mapper.getImageStruct(image+":"+tag, tags, response[0].OwnerUser),
	}, nil
}
func (mapper *PortodshimImageMapper) PullImage(ctx context.Context, req *v1alpha2.PullImageRequest) (*v1alpha2.PullImageResponse, error) {
	// TODO: Убрать заглушку
	return &v1alpha2.PullImageResponse{
		ImageRef: req.Image.GetImage(),
	}, nil
}
func (mapper *PortodshimImageMapper) RemoveImage(ctx context.Context, req *v1alpha2.RemoveImageRequest) (*v1alpha2.RemoveImageResponse, error) {
	// temporarily
	//err := ctx.Value("portoClient").(API).RemoveLayer(req.Image.GetImage())
	//if err != nil {
	//	return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	//}

	return &v1alpha2.RemoveImageResponse{}, nil
}
func (mapper *PortodshimImageMapper) ImageFsInfo(ctx context.Context, req *v1alpha2.ImageFsInfoRequest) (*v1alpha2.ImageFsInfoResponse, error) {
	stat := syscall.Statfs_t{}
	err := syscall.Statfs(LayersPath, &stat)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	return &v1alpha2.ImageFsInfoResponse{
		ImageFilesystems: []*v1alpha2.FilesystemUsage{
			{
				Timestamp: time.Now().UnixNano(),
				FsId: &v1alpha2.FilesystemIdentifier{
					Mountpoint: LayersPath,
				},
				UsedBytes:  &v1alpha2.UInt64Value{Value: (stat.Blocks - stat.Bfree) * uint64(stat.Bsize)},
				InodesUsed: &v1alpha2.UInt64Value{Value: stat.Files - stat.Ffree},
			},
		},
	}, nil
}
