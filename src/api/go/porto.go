package porto

import (
	"encoding/binary"
	"fmt"
	"io"
	"math"
	"net"
	"syscall"
	"time"

	"github.com/golang/protobuf/proto"

	"github.com/yandex/porto/src/api/go/rpc"
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

type TVolumeDescription struct {
	Path       string
	Properties map[string]string
	Containers []string
}

type TPortoGetResponse struct {
	Value    string
	Error    int
	ErrorMsg string
}

type Error struct {
	Errno   rpc.EError
	ErrName string
	Message string
}

func (e *Error) Error() string {
	return fmt.Sprintf("[%d] %s: %s", e.Errno, e.ErrName, e.Message)
}

type API interface {
	SetTimeout(time.Duration)

	GetVersion() (string, string, error)

	GetLastError() rpc.EError
	GetLastErrorMessage() string

	// ContainerAPI
	Create(name string) error
	Destroy(name string) error

	Start(name string) error
	Stop(name string) error
	Kill(name string, sig syscall.Signal) error
	Pause(name string) error
	Resume(name string) error

	Wait(containers []string, timeout time.Duration) (string, error)

	List() ([]string, error)
	Plist() ([]TProperty, error)
	Dlist() ([]TData, error)

	Get(containers []string, variables []string) (map[string]map[string]TPortoGetResponse, error)

	GetProperty(name string, property string) (string, error)
	SetProperty(name string, property string, value string) error

	GetData(name string, data string) (string, error)

	// VolumeAPI
	ListVolumeProperties() ([]TProperty, error)
	CreateVolume(path string, config map[string]string) (TVolumeDescription, error)
	LinkVolume(path string, container string) error
	UnlinkVolume(path string, container string) error
	ListVolumes(path string, container string) ([]TVolumeDescription, error)

	// LayerAPI
	ImportLayer(layer string, tarball string, merge bool) error
	ExportLayer(volume string, tarball string) error
	RemoveLayer(layer string) error
	ListLayers() ([]string, error)

	ConvertPath(path string, src string, dest string) (string, error)

	Close() error
}

type portoConnection struct {
	conn net.Conn
	err  rpc.EError
	msg  string

	timeout time.Duration
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

// SetTimeout sets timeout for all operations
// Passing zero timeout cleans this behavior
func (conn *portoConnection) SetTimeout(timeout time.Duration) {
	conn.timeout = timeout
}

func (conn *portoConnection) Close() error {
	return conn.conn.Close()
}

func (conn *portoConnection) GetLastError() rpc.EError {
	return conn.err
}

func (conn *portoConnection) GetLastErrorMessage() string {
	return conn.msg
}

func (conn *portoConnection) GetVersion() (string, string, error) {
	req := &rpc.TContainerRequest{
		Version: new(rpc.TVersionRequest),
	}
	resp, err := conn.performRequest(req)
	if err != nil {
		return "", "", err
	}

	return resp.GetVersion().GetTag(), resp.GetVersion().GetRevision(), nil
}

func (conn *portoConnection) performRequest(req *rpc.TContainerRequest) (*rpc.TContainerResponse, error) {
	conn.err = 0
	conn.msg = ""

	data, err := proto.Marshal(req)
	if err != nil {
		return nil, err
	}

	if conn.timeout != 0 {
		conn.conn.SetDeadline(time.Now().Add(conn.timeout))
		// passing zero time should
		var zeroTime time.Time
		defer conn.conn.SetDeadline(zeroTime)
	}

	err = sendData(conn.conn, data)
	if err != nil {
		return nil, err
	}

	data, err = recvData(conn.conn)
	if err != nil {
		return nil, err
	}

	resp := new(rpc.TContainerResponse)

	err = proto.Unmarshal(data, resp)
	if err != nil {
		return nil, err
	}

	conn.err = resp.GetError()
	conn.msg = resp.GetErrorMsg()

	if resp.GetError() != rpc.EError_Success {
		return resp, &Error{
			Errno:   conn.err,
			ErrName: rpc.EError_name[int32(conn.err)],
			Message: conn.msg,
		}
	}

	return resp, nil
}

// ContainerAPI
func (conn *portoConnection) Create(name string) error {
	req := &rpc.TContainerRequest{
		Create: &rpc.TContainerCreateRequest{
			Name: &name,
		},
	}
	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) Destroy(name string) error {
	req := &rpc.TContainerRequest{
		Destroy: &rpc.TContainerDestroyRequest{
			Name: &name,
		},
	}
	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) Start(name string) error {
	req := &rpc.TContainerRequest{
		Start: &rpc.TContainerStartRequest{
			Name: &name,
		},
	}
	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) Stop(name string) error {
	req := &rpc.TContainerRequest{
		Stop: &rpc.TContainerStopRequest{
			Name: &name,
		},
	}
	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) Kill(name string, sig syscall.Signal) error {
	signum := int32(sig)
	req := &rpc.TContainerRequest{
		Kill: &rpc.TContainerKillRequest{
			Name: &name,
			Sig:  &signum,
		},
	}
	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) Pause(name string) error {
	req := &rpc.TContainerRequest{
		Pause: &rpc.TContainerPauseRequest{
			Name: &name,
		},
	}
	_, err := conn.performRequest(req)
	return err

}

func (conn *portoConnection) Resume(name string) error {
	req := &rpc.TContainerRequest{
		Resume: &rpc.TContainerResumeRequest{
			Name: &name,
		},
	}
	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) Wait(containers []string, timeout time.Duration) (string, error) {
	req := &rpc.TContainerRequest{
		Wait: &rpc.TContainerWaitRequest{
			Name: containers,
		},
	}

	if timeout >= 0 {
		if timeout/time.Millisecond > math.MaxUint32 {
			return "", fmt.Errorf("timeout must be less than %d ms", math.MaxUint32)
		}

		timeoutms := uint32(timeout / time.Millisecond)
		req.Wait.Timeout = &timeoutms
	}

	resp, err := conn.performRequest(req)
	if err != nil {
		return "", err
	}

	return resp.GetWait().GetName(), nil
}

func (conn *portoConnection) List() ([]string, error) {
	req := &rpc.TContainerRequest{
		List: new(rpc.TContainerListRequest),
	}
	resp, err := conn.performRequest(req)
	if err != nil {
		return nil, err
	}

	return resp.GetList().GetName(), nil
}

func (conn *portoConnection) Plist() (ret []TProperty, err error) {
	req := &rpc.TContainerRequest{
		PropertyList: new(rpc.TContainerPropertyListRequest),
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
	req := &rpc.TContainerRequest{
		DataList: new(rpc.TContainerDataListRequest),
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
	ret = make(map[string]map[string]TPortoGetResponse)
	req := &rpc.TContainerRequest{
		Get: &rpc.TContainerGetRequest{
			Name:     containers,
			Variable: variables,
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
	req := &rpc.TContainerRequest{
		GetProperty: &rpc.TContainerGetPropertyRequest{
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
	req := &rpc.TContainerRequest{
		SetProperty: &rpc.TContainerSetPropertyRequest{
			Name:     &name,
			Property: &property,
			Value:    &value,
		},
	}
	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) GetData(name string, data string) (string, error) {
	req := &rpc.TContainerRequest{
		GetData: &rpc.TContainerGetDataRequest{
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
	req := &rpc.TContainerRequest{
		ListVolumeProperties: &rpc.TVolumePropertyListRequest{},
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

func (conn *portoConnection) CreateVolume(path string, config map[string]string) (desc TVolumeDescription, err error) {
	var properties []*rpc.TVolumeProperty
	for k, v := range config {
		// NOTE: `k`, `v` save their addresses during `range`.
		// If we append pointers to them into an array,
		// all elements in the array will be the same.
		// So a pointer to the copy must be used.
		name, value := k, v
		prop := &rpc.TVolumeProperty{Name: &name, Value: &value}
		properties = append(properties, prop)
	}

	req := &rpc.TContainerRequest{
		CreateVolume: &rpc.TVolumeCreateRequest{
			Path:       &path,
			Properties: properties,
		},
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

func (conn *portoConnection) LinkVolume(path string, container string) error {
	req := &rpc.TContainerRequest{
		LinkVolume: &rpc.TVolumeLinkRequest{
			Path:      &path,
			Container: &container,
		},
	}
	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) UnlinkVolume(path string, container string) error {
	req := &rpc.TContainerRequest{
		UnlinkVolume: &rpc.TVolumeUnlinkRequest{
			Path:      &path,
			Container: &container,
		},
	}
	if container == "" {
		req.UnlinkVolume.Container = nil
	}
	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) ListVolumes(path string, container string) (ret []TVolumeDescription, err error) {
	req := &rpc.TContainerRequest{
		ListVolumes: &rpc.TVolumeListRequest{
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
		var desc TVolumeDescription
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
	req := &rpc.TContainerRequest{
		ImportLayer: &rpc.TLayerImportRequest{
			Layer:   &layer,
			Tarball: &tarball,
			Merge:   &merge},
	}
	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) ExportLayer(volume string, tarball string) error {
	req := &rpc.TContainerRequest{
		ExportLayer: &rpc.TLayerExportRequest{
			Volume:  &volume,
			Tarball: &tarball,
		},
	}
	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) RemoveLayer(layer string) error {
	req := &rpc.TContainerRequest{
		RemoveLayer: &rpc.TLayerRemoveRequest{
			Layer: &layer,
		},
	}
	_, err := conn.performRequest(req)
	return err
}

func (conn *portoConnection) ListLayers() ([]string, error) {
	req := &rpc.TContainerRequest{
		ListLayers: &rpc.TLayerListRequest{},
	}

	resp, err := conn.performRequest(req)
	if err != nil {
		return nil, err
	}

	return resp.GetLayers().GetLayer(), nil
}

func (conn *portoConnection) ConvertPath(path string, src string, dest string) (string, error) {
	req := &rpc.TContainerRequest{
		ConvertPath: &rpc.TConvertPathRequest{
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
