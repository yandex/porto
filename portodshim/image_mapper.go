package main

import (
	"context"
	"fmt"
	"syscall"
	"time"

	v1alpha2 "k8s.io/cri-api/pkg/apis/runtime/v1alpha2"
)

const (
	LayersPath = "/place/porto_layers"
)

type PortodshimImageMapper struct {
	portoClient API
}

// IMAGE SERVICE INTERFACE
func (mapper *PortodshimImageMapper) ListImages(ctx context.Context, req *v1alpha2.ListImagesRequest) (*v1alpha2.ListImagesResponse, error) {
	response, err := ctx.Value("portoClient").(API).ListLayers2("", "")
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}

	var images []*v1alpha2.Image
	for _, desc := range response {
		images = append(images, &v1alpha2.Image{
			Id:       desc.Name,
			Username: desc.OwnerUser,
		})
	}

	return &v1alpha2.ListImagesResponse{
		Images: images,
	}, nil
}
func (mapper *PortodshimImageMapper) ImageStatus(ctx context.Context, req *v1alpha2.ImageStatusRequest) (*v1alpha2.ImageStatusResponse, error) {
	response, err := ctx.Value("portoClient").(API).ListLayers2("", req.Image.GetImage())
	if err != nil {
		return nil, fmt.Errorf("%s: %v", getCurrentFuncName(), err)
	}
	if len(response) == 0 {
		return nil, fmt.Errorf("%s: layer not found", getCurrentFuncName())
	}
	if len(response) > 1 {
		return nil, fmt.Errorf("%s: too many layers found", getCurrentFuncName())
	}

	return &v1alpha2.ImageStatusResponse{
		Image: &v1alpha2.Image{
			Id:       response[0].Name,
			Username: response[0].OwnerUser,
		},
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
	//err = ctx.Value("portoClient").(API).RemoveLayer(req.Image.GetImage())
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
