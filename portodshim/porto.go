package main

import (
	"encoding/binary"
	"fmt"
	"google.golang.org/protobuf/proto"
	"io"
	"math"
	"net"
	"syscall"
	"time"
)

const portoSocket = "/run/portod.socket"

func sendData(conn io.Writer, data []byte) error {
	// First we have to send actual data size,
	// then the data itself
	buf := make([]byte, 64)
	len := binary.PutUvarint(buf, uint64(len(data)))
	_, err := conn.Write(buf[:len])
	if err != nil {
		return err
	}
	_, err = conn.Write(data)
	return err
}

func recvData(conn io.Reader) ([]byte, error) {
	buf := make([]byte, 1024*1024)

	size, err := conn.Read(buf)
	if err != nil {
		return nil, err
	}

	exp, shift := binary.Uvarint(buf)

	// length of result is exp,
	// so preallocate a buffer for it.
	var ret = make([]byte, exp)
	// bytes after an encoded uint64 and up to len
	// are belong to a packed structure, so copy them
	copy(ret, buf[shift:size])

	// we don't need to check that
	// size > shift, as we ask to read enough data
	// to decode uint64. Otherwise we would have an error before.
	for pos := size - shift; uint64(pos) < exp; {
		n, err := conn.Read(ret[pos:])
		if err != nil {
			return nil, err
		}
		pos += n
	}

	return ret, nil
}

type TProperty struct {
	Name        string
	Description string
}

type TData struct {
	Name        string
	Description string
}

type TPortoVolumeDescription struct {
	Path       string
	Properties map[string]string
	Containers []string
}

type TPortoStorageDescription struct {
	Name         string
	OwnerUser    string
	OwnerGroup   string
	LastUsage    uint64
	PrivateValue string
}

type TPortoLayerDescription struct {
	Name         string
	OwnerUser    string
	OwnerGroup   string
	LastUsage    uint64
	PrivateValue string
}

type TPortoGetResponse struct {
	Value    string
	Error    int
	ErrorMsg string
}

type Error struct {
	Errno   EError
	ErrName string
	Message string
}

func (e *Error) Error() string {
	return fmt.Sprintf("[%d] %s: %s", e.Errno, e.ErrName, e.Message)
}

type API interface {
	GetVersion() (string, string, error)

	GetLastError() EError
	GetLastErrorMessage() string

	// ContainerAPI
	Create(name string) error
	CreateWeak(name string) error
	Destroy(name string) error

	Start(name string) error
	Stop(name string) error
	Stop2(name string, timeout time.Duration) error
	Kill(name string, sig syscall.Signal) error
	Pause(name string) error
	Resume(name string) error

	Wait(containers []string, timeout time.Duration) (string, error)

	List() ([]string, error)
	List1(mask string) ([]string, error)
	Plist() ([]TProperty, error)
	Dlist() ([]TData, error)

	Get(containers []string, variables []string) (map[string]map[string]TPortoGetResponse, error)
	Get3(containers []string, variables []string, nonblock bool) (
		map[string]map[string]TPortoGetResponse, error)

	GetProperty(name string, property string) (string, error)
	SetProperty(name string, property string, value string) error

	GetData(name string, data string) (string, error)

	// VolumeAPI
	ListVolumeProperties() ([]TProperty, error)
	CreateVolume(path string, config map[string]string) (TPortoVolumeDescription, error)
	TuneVolume(path string, config map[string]string) error
	LinkVolume(path string, container string) error
	UnlinkVolume(path string, container string) error
	UnlinkVolume3(path string, container string, strict bool) error
	ListVolumes(path string, container string) ([]TPortoVolumeDescription, error)

	// LayerAPI
	ImportLayer(layer string, tarball string, merge bool) error
	ImportLayer4(layer string, tarball string, merge bool,
		place string, privateValue string) error
	ExportLayer(volume string, tarball string) error
	RemoveLayer(layer string) error
	RemoveLayer2(layer string, place string) error
	ListLayers() ([]string, error)
	ListLayers2(place string, mask string) ([]TPortoLayerDescription, error)

	GetLayerPrivate(layer string, place string) (string, error)
	SetLayerPrivate(layer string, place string, privateValue string) error

	ListStorage(place string, mask string) ([]TPortoStorageDescription, error)
	RemoveStorage(name string, place string) error

	ConvertPath(path string, src string, dest string) (string, error)
	AttachProcess(name string, pid uint32, comm string) error

	Close() error
}

type portoConnection struct {
	conn net.Conn
	err  EError
	msg  string
}

//Connect establishes connection to a Porto daemon via unix socket.
//Close must be called when the API is not needed anymore.
func Connect() (API, error) {
	c, err := net.Dial("unix", portoSocket)
	if err != nil {
		return nil, err
	}

	ret := new(portoConnection)
	ret.conn = c
	return ret, nil
}

func (conn *portoConnection) Close() error {
	return conn.conn.Close()
}

func (conn *portoConnection) GetLastError() EError {
	return conn.err
}

func (conn *portoConnection) GetLastErrorMessage() string {
	return conn.msg
}

func (conn *portoConnection) GetVersion() (string, string, error) {
	req := &TContainerRequest{
		Version: new(TVersionRequest),
	}
	resp, err := conn.performRequest(req)
	if err != nil {
		return "", "", err
	}

	return resp.GetVersion().GetTag(), resp.GetVersion().GetRevision(), nil
}

func (conn *portoConnection) performRequest(req *TContainerRequest) (*TContainerResponse, error) {
	conn.err = 0
	conn.msg = ""

	data, err := proto.Marshal(req)
	if err != nil {
		return nil, err
	}

	err = sendData(conn.conn, data)
	if err != nil {
		return nil, err
	}

	data, err = recvData(conn.conn)
	if err != nil {
		return nil, err
	}

	resp := new(TContainerResponse)

	err = proto.Unmarshal(data, resp)
	if err != nil {
		return nil, err
	}

	conn.err = resp.GetError()
	conn.msg = resp.GetErrorMsg()

	if resp.GetError() != EError_Success {
		return resp, &Error{
			Errno:   conn.err,
			ErrName: EError_name[int32(conn.err)],
			Message: conn.msg,
		}
	}

	return resp, nil
}

// ContainerAPI
func (conn *portoConnection) Create(name string) error {
	req := &TContainerRequest{
		Create: &TContainerCreateRequest{
			Name: &name,
		},
	}
	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) CreateWeak(name string) error {
	req := &TContainerRequest{
		CreateWeak: &TContainerCreateRequest{
			Name: &name,
		},
	}
	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) Destroy(name string) error {
	req := &TContainerRequest{
		Destroy: &TContainerDestroyRequest{
			Name: &name,
		},
	}
	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) Start(name string) error {
	req := &TContainerRequest{
		Start: &TContainerStartRequest{
			Name: &name,
		},
	}
	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) Stop(name string) error {
	return conn.Stop2(name, -1)
}

func (conn *portoConnection) Stop2(name string, timeout time.Duration) error {
	req := &TContainerRequest{
		Stop: &TContainerStopRequest{
			Name: &name,
		},
	}

	if timeout >= 0 {
		if timeout/time.Millisecond > math.MaxUint32 {
			return fmt.Errorf("timeout must be less than %d ms", math.MaxUint32)
		}

		timeoutms := uint32(timeout / time.Millisecond)
		req.Stop.TimeoutMs = &timeoutms
	}

	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) Kill(name string, sig syscall.Signal) error {
	signum := int32(sig)
	req := &TContainerRequest{
		Kill: &TContainerKillRequest{
			Name: &name,
			Sig:  &signum,
		},
	}
	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) Pause(name string) error {
	req := &TContainerRequest{
		Pause: &TContainerPauseRequest{
			Name: &name,
		},
	}
	_, err := conn.performRequest(req)
	return err

}

func (conn *portoConnection) Resume(name string) error {
	req := &TContainerRequest{
		Resume: &TContainerResumeRequest{
			Name: &name,
		},
	}
	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) Wait(containers []string, timeout time.Duration) (string, error) {
	req := &TContainerRequest{
		Wait: &TContainerWaitRequest{
			Name: containers,
		},
	}

	if timeout >= 0 {
		if timeout/time.Millisecond > math.MaxUint32 {
			return "", fmt.Errorf("timeout must be less than %d ms", math.MaxUint32)
		}

		timeoutms := uint32(timeout / time.Millisecond)
		req.Wait.TimeoutMs = &timeoutms
	}

	resp, err := conn.performRequest(req)
	if err != nil {
		return "", err
	}

	return resp.GetWait().GetName(), nil
}

func (conn *portoConnection) List() ([]string, error) {
	return conn.List1("")
}

func (conn *portoConnection) List1(mask string) ([]string, error) {
	req := &TContainerRequest{
		List: &TContainerListRequest{},
	}

	if mask != "" {
		req.List.Mask = &mask
	}

	resp, err := conn.performRequest(req)
	if err != nil {
		return nil, err
	}

	return resp.GetList().GetName(), nil
}

func (conn *portoConnection) Plist() (ret []TProperty, err error) {
	req := &TContainerRequest{
		PropertyList: new(TContainerPropertyListRequest),
	}
	resp, err := conn.performRequest(req)
	for _, property := range resp.GetPropertyList().GetList() {
		var p = TProperty{
			Name:        property.GetName(),
			Description: property.GetDesc(),
		}
		ret = append(ret, p)
	}
	return ret, err
}

func (conn *portoConnection) Dlist() (ret []TData, err error) {
	req := &TContainerRequest{
		DataList: new(TContainerDataListRequest),
	}
	resp, err := conn.performRequest(req)
	if err != nil {
		return nil, err
	}

	for _, data := range resp.GetDataList().GetList() {
		var p = TData{
			Name:        data.GetName(),
			Description: data.GetDesc(),
		}
		ret = append(ret, p)
	}

	return ret, nil
}

func (conn *portoConnection) Get(containers []string, variables []string) (ret map[string]map[string]TPortoGetResponse, err error) {
	return conn.Get3(containers, variables, false)
}

func (conn *portoConnection) Get3(containers []string, variables []string, nonblock bool) (ret map[string]map[string]TPortoGetResponse, err error) {
	ret = make(map[string]map[string]TPortoGetResponse)
	req := &TContainerRequest{
		Get: &TContainerGetRequest{
			Name:     containers,
			Variable: variables,
			Nonblock: &nonblock,
		},
	}

	resp, err := conn.performRequest(req)
	if err != nil {
		return nil, err
	}

	for _, item := range resp.GetGet().GetList() {
		for _, value := range item.GetKeyval() {
			var v = TPortoGetResponse{
				Value:    value.GetValue(),
				Error:    int(value.GetError()),
				ErrorMsg: value.GetErrorMsg(),
			}

			if _, ok := ret[item.GetName()]; !ok {
				ret[item.GetName()] = make(map[string]TPortoGetResponse)
			}

			ret[item.GetName()][value.GetVariable()] = v
		}
	}
	return ret, err
}

func (conn *portoConnection) GetProperty(name string, property string) (string, error) {
	req := &TContainerRequest{
		GetProperty: &TContainerGetPropertyRequest{
			Name:     &name,
			Property: &property,
		},
	}

	resp, err := conn.performRequest(req)
	if err != nil {
		return "", err
	}

	return resp.GetGetProperty().GetValue(), nil
}

func (conn *portoConnection) SetProperty(name string, property string, value string) error {
	req := &TContainerRequest{
		SetProperty: &TContainerSetPropertyRequest{
			Name:     &name,
			Property: &property,
			Value:    &value,
		},
	}
	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) GetData(name string, data string) (string, error) {
	req := &TContainerRequest{
		GetData: &TContainerGetDataRequest{
			Name: &name,
			Data: &data,
		},
	}

	resp, err := conn.performRequest(req)
	if err != nil {
		return "", err
	}

	return resp.GetGetData().GetValue(), nil
}

// VolumeAPI
func (conn *portoConnection) ListVolumeProperties() (ret []TProperty, err error) {
	req := &TContainerRequest{
		ListVolumeProperties: &TVolumePropertyListRequest{},
	}

	resp, err := conn.performRequest(req)
	if err != nil {
		return nil, err
	}

	for _, property := range resp.GetVolumePropertyList().GetProperties() {
		var desc = TProperty{
			Name:        property.GetName(),
			Description: property.GetDesc(),
		}
		ret = append(ret, desc)
	}
	return ret, err
}

func (conn *portoConnection) CreateVolume(path string, config map[string]string) (desc TPortoVolumeDescription, err error) {
	var properties []*TVolumeProperty
	for k, v := range config {
		// NOTE: `k`, `v` save their addresses during `range`.
		// If we append pointers to them into an array,
		// all elements in the array will be the same.
		// So a pointer to the copy must be used.
		name, value := k, v
		prop := &TVolumeProperty{Name: &name, Value: &value}
		properties = append(properties, prop)
	}

	req := &TContainerRequest{
		CreateVolume: &TVolumeCreateRequest{
			Properties: properties,
		},
	}

	if path != "" {
		req.CreateVolume.Path = &path
	}

	resp, err := conn.performRequest(req)
	if err != nil {
		return desc, err
	}

	volume := resp.GetVolume()
	desc.Path = volume.GetPath()
	desc.Containers = append(desc.Containers, volume.GetContainers()...)
	desc.Properties = make(map[string]string, len(volume.GetProperties()))

	for _, property := range volume.GetProperties() {
		k := property.GetName()
		v := property.GetValue()
		desc.Properties[k] = v
	}

	return desc, err
}

func (conn *portoConnection) TuneVolume(path string, config map[string]string) error {
	var properties []*TVolumeProperty
	for k, v := range config {
		name, value := k, v
		prop := &TVolumeProperty{Name: &name, Value: &value}
		properties = append(properties, prop)
	}
	req := &TContainerRequest{
		TuneVolume: &TVolumeTuneRequest{
			Path:       &path,
			Properties: properties,
		},
	}
	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) LinkVolume(path string, container string) error {
	req := &TContainerRequest{
		LinkVolume: &TVolumeLinkRequest{
			Path:      &path,
			Container: &container,
		},
	}
	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) UnlinkVolume(path string, container string) error {
	return conn.UnlinkVolume3(path, container, false)
}

func (conn *portoConnection) UnlinkVolume3(path string, container string, strict bool) error {
	req := &TContainerRequest{
		UnlinkVolume: &TVolumeUnlinkRequest{
			Path:      &path,
			Container: &container,
			Strict:    &strict,
		},
	}
	if container == "" {
		req.UnlinkVolume.Container = nil
	}
	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) ListVolumes(path string, container string) (ret []TPortoVolumeDescription, err error) {
	req := &TContainerRequest{
		ListVolumes: &TVolumeListRequest{
			Path:      &path,
			Container: &container,
		},
	}

	if path == "" {
		req.ListVolumes.Path = nil
	}
	if container == "" {
		req.ListVolumes.Container = nil
	}

	resp, err := conn.performRequest(req)
	if err != nil {
		return nil, err
	}

	for _, volume := range resp.GetVolumeList().GetVolumes() {
		var desc TPortoVolumeDescription
		desc.Path = volume.GetPath()
		desc.Containers = append(desc.Containers, volume.GetContainers()...)
		desc.Properties = make(map[string]string)

		for _, property := range volume.GetProperties() {
			k := property.GetName()
			v := property.GetValue()
			desc.Properties[k] = v
		}
		ret = append(ret, desc)
	}
	return ret, err
}

// LayerAPI
func (conn *portoConnection) ImportLayer(layer string, tarball string, merge bool) error {
	return conn.ImportLayer4(layer, tarball, merge, "", "")
}

func (conn *portoConnection) ImportLayer4(layer string, tarball string, merge bool,
	place string, privateValue string) error {
	req := &TContainerRequest{
		ImportLayer: &TLayerImportRequest{
			Layer:        &layer,
			Tarball:      &tarball,
			Merge:        &merge,
			PrivateValue: &privateValue,
		},
	}

	if place != "" {
		req.ImportLayer.Place = &place
	}

	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) ExportLayer(volume string, tarball string) error {
	req := &TContainerRequest{
		ExportLayer: &TLayerExportRequest{
			Volume:  &volume,
			Tarball: &tarball,
		},
	}
	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) RemoveLayer(layer string) error {
	return conn.RemoveLayer2(layer, "")
}

func (conn *portoConnection) RemoveLayer2(layer string, place string) error {
	req := &TContainerRequest{
		RemoveLayer: &TLayerRemoveRequest{
			Layer: &layer,
		},
	}

	if place != "" {
		req.RemoveLayer.Place = &place
	}

	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) ListLayers() ([]string, error) {
	req := &TContainerRequest{
		ListLayers: &TLayerListRequest{},
	}

	resp, err := conn.performRequest(req)
	if err != nil {
		return nil, err
	}

	return resp.GetLayers().GetLayer(), nil
}

func (conn *portoConnection) ListLayers2(place string, mask string) (ret []TPortoLayerDescription, err error) {
	req := &TContainerRequest{
		ListLayers: &TLayerListRequest{},
	}

	if place != "" {
		req.ListLayers.Place = &place
	}

	if mask != "" {
		req.ListLayers.Mask = &mask
	}

	resp, err := conn.performRequest(req)
	if err != nil {
		return nil, err
	}

	for _, layer := range resp.GetLayers().GetLayers() {
		var desc TPortoLayerDescription

		desc.Name = layer.GetName()
		desc.OwnerUser = layer.GetOwnerUser()
		desc.OwnerGroup = layer.GetOwnerGroup()
		desc.LastUsage = layer.GetLastUsage()
		desc.PrivateValue = layer.GetPrivateValue()

		ret = append(ret, desc)
	}

	return ret, nil
}

func (conn *portoConnection) GetLayerPrivate(layer string, place string) (string, error) {
	req := &TContainerRequest{
		Getlayerprivate: &TLayerGetPrivateRequest{
			Layer: &layer,
		},
	}

	if place != "" {
		req.Getlayerprivate.Place = &place
	}

	resp, err := conn.performRequest(req)
	if err != nil {
		return "", err
	}

	return resp.GetLayerPrivate().GetPrivateValue(), nil
}

func (conn *portoConnection) SetLayerPrivate(layer string, place string,
	privateValue string) error {
	req := &TContainerRequest{
		Setlayerprivate: &TLayerSetPrivateRequest{
			Layer:        &layer,
			PrivateValue: &privateValue,
		},
	}

	if place != "" {
		req.Setlayerprivate.Place = &place
	}

	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) ListStorage(place string, mask string) (ret []TPortoStorageDescription, err error) {
	req := &TContainerRequest{
		ListStorage: &TStorageListRequest{},
	}

	if place != "" {
		req.ListStorage.Place = &place
	}

	if mask != "" {
		req.ListStorage.Mask = &mask
	}

	resp, err := conn.performRequest(req)
	if err != nil {
		return nil, err
	}

	for _, storage := range resp.GetStorageList().GetStorages() {
		var desc TPortoStorageDescription

		desc.Name = storage.GetName()
		desc.OwnerUser = storage.GetOwnerUser()
		desc.OwnerGroup = storage.GetOwnerGroup()
		desc.LastUsage = storage.GetLastUsage()
		desc.PrivateValue = storage.GetPrivateValue()

		ret = append(ret, desc)
	}

	return ret, nil
}

func (conn *portoConnection) RemoveStorage(name string, place string) error {
	req := &TContainerRequest{
		RemoveStorage: &TStorageRemoveRequest{
			Name: &name,
		},
	}

	if place != "" {
		req.RemoveStorage.Place = &place
	}

	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) ConvertPath(path string, src string, dest string) (string, error) {
	req := &TContainerRequest{
		ConvertPath: &TConvertPathRequest{
			Path:        &path,
			Source:      &src,
			Destination: &dest,
		},
	}

	resp, err := conn.performRequest(req)
	if err != nil {
		return "", err
	}

	return resp.GetConvertPath().GetPath(), nil
}

func (conn *portoConnection) AttachProcess(name string, pid uint32, comm string) error {
	req := &TContainerRequest{
		AttachProcess: &TAttachProcessRequest{
			Name: &name,
			Pid:  &pid,
			Comm: &comm,
		},
	}

	_, err := conn.performRequest(req)
	return err
}
