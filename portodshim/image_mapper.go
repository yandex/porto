package main

import (
	"context"
	"fmt"
	v1alpha2 "k8s.io/cri-api/pkg/apis/runtime/v1alpha2"
)

type PortodshimImageMapper struct {
	portoClient API
}

// IMAGE SERVICE INTERFACE
func (mapper *PortodshimImageMapper) ListImages(ctx context.Context, req *v1alpha2.ListImagesRequest) (*v1alpha2.ListImagesResponse, error) {
	return nil, fmt.Errorf("not implemented ListImages")
}
func (mapper *PortodshimImageMapper) ImageStatus(ctx context.Context, req *v1alpha2.ImageStatusRequest) (*v1alpha2.ImageStatusResponse, error) {
	return nil, fmt.Errorf("not implemented ImageStatus")
}
func (mapper *PortodshimImageMapper) PullImage(ctx context.Context, req *v1alpha2.PullImageRequest) (*v1alpha2.PullImageResponse, error) {
	return nil, fmt.Errorf("not implemented PullImage")
}
func (mapper *PortodshimImageMapper) RemoveImage(ctx context.Context, req *v1alpha2.RemoveImageRequest) (*v1alpha2.RemoveImageResponse, error) {
	return nil, fmt.Errorf("not implemented RemoveImage")
}
func (mapper *PortodshimImageMapper) ImageFsInfo(ctx context.Context, req *v1alpha2.ImageFsInfoRequest) (*v1alpha2.ImageFsInfoResponse, error) {
	return nil, fmt.Errorf("not implemented ImageFsInfo")
}
