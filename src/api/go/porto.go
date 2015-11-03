package porto

import (
	"encoding/binary"
	"errors"
	"net"

	"github.com/golang/protobuf/proto"

	"github.com/yandex/porto/src/api/go/rpc"
)

const PortoSocket = "/var/run/portod.socket"

func SendData(conn net.Conn, data []byte) error {
	buf := make([]byte, 64)
	len := binary.PutUvarint(buf, uint64(len(data)))
	_, err := conn.Write(buf[0:len])
	if err != nil {
		return err
	}
	_, err = conn.Write(data)
	return err
}

func RecvData(conn net.Conn) ([]byte, error) {
	buf := make([]byte, 1024*1024)

	len, err := conn.Read(buf)
	if err != nil {
		return nil, err
	}
	exp, shift := binary.Uvarint(buf)

	ret := buf[shift:]

	for uint64(len+shift) < exp {
		tmp, err := conn.Read(buf)
		if err != nil {
			return nil, err
		}
		len += tmp
		ret = append(ret, buf...)
	}

	return ret, nil
}

func (conn *PortoConnection) PerformRequest(req *rpc.TContainerRequest) (*rpc.TContainerResponse, error) {
	conn.err = 0
	conn.msg = ""

	data, err := proto.Marshal(req)
	if err != nil {
		return nil, err
	}

	err = SendData(conn.conn, data)
	if err != nil {
		return nil, err
	}

	data, err = RecvData(conn.conn)
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
		return resp, errors.New(rpc.EError_name[int32(resp.GetError())])
	}

	return resp, nil
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

type PortoAPI interface {
	GetVersion() (string, string, error)

	GetLastError() rpc.EError
	GetLastErrorMessage() string

	// ContainerAPI
	Create(name string) error
	Destroy(name string) error

	Start(name string) error
	Stop(name string) error
	Kill(name string, sig int) error
	Pause(name string) error
	Resume(name string) error

	Wait(containers []string, timeout int) (string, error)

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
	ListLayers(layers []string) error
}

type PortoConnection struct {
	conn net.Conn
	err  rpc.EError
	msg  string
}

func NewPortoConnection() (*PortoConnection, error) {
	c, err := net.Dial("unix", PortoSocket)
	if err != nil {
		return nil, err
	}

	ret := new(PortoConnection)
	ret.conn = c
	return ret, nil
}

func (conn *PortoConnection) Close() error {
	return conn.conn.Close()
}

func (conn *PortoConnection) GetLastError() rpc.EError {
	return conn.err
}

func (conn *PortoConnection) GetLastErrorMessage() string {
	return conn.msg
}

func (conn *PortoConnection) GetVersion() (string, string, error) {
	req := &rpc.TContainerRequest{Version: new(rpc.TVersionRequest)}
	resp, err := conn.PerformRequest(req)
	if err != nil {
		return "", "", err
	}

	return resp.GetVersion().GetTag(), resp.GetVersion().GetRevision(), nil
}

// ContainerAPI
func (conn *PortoConnection) Create(name string) error {
	req := &rpc.TContainerRequest{Create: &rpc.TContainerCreateRequest{Name: &name}}
	_, err := conn.PerformRequest(req)
	return err
}

func (conn *PortoConnection) Destroy(name string) error {
	req := &rpc.TContainerRequest{Destroy: &rpc.TContainerDestroyRequest{Name: &name}}
	_, err := conn.PerformRequest(req)
	return err
}

func (conn *PortoConnection) Start(name string) error {
	req := &rpc.TContainerRequest{Start: &rpc.TContainerStartRequest{Name: &name}}
	_, err := conn.PerformRequest(req)
	return err
}

func (conn *PortoConnection) Stop(name string) error {
	req := &rpc.TContainerRequest{Stop: &rpc.TContainerStopRequest{Name: &name}}
	_, err := conn.PerformRequest(req)
	return err
}

func (conn *PortoConnection) Kill(name string, sig int32) error {
	req := &rpc.TContainerRequest{Kill: &rpc.TContainerKillRequest{Name: &name, Sig: &sig}}
	_, err := conn.PerformRequest(req)
	return err
}

func (conn *PortoConnection) Pause(name string) error {
	req := &rpc.TContainerRequest{Pause: &rpc.TContainerPauseRequest{Name: &name}}
	_, err := conn.PerformRequest(req)
	return err

}

func (conn *PortoConnection) Resume(name string) error {
	req := &rpc.TContainerRequest{Resume: &rpc.TContainerResumeRequest{Name: &name}}
	_, err := conn.PerformRequest(req)
	return err
}

func (conn *PortoConnection) Wait(containers []string, timeout int) (string, error) {
	req := &rpc.TContainerRequest{Wait: &rpc.TContainerWaitRequest{Name: containers}}
	resp, err := conn.PerformRequest(req)
	if err != nil {
		return "", err
	}

	return resp.GetWait().GetName(), nil
}

func (conn *PortoConnection) List() ([]string, error) {
	req := &rpc.TContainerRequest{List: new(rpc.TContainerListRequest)}
	resp, err := conn.PerformRequest(req)
	if err != nil {
		return nil, err
	}

	return resp.GetList().GetName(), nil
}

func (conn *PortoConnection) Plist() (ret []TProperty, err error) {
	req := &rpc.TContainerRequest{PropertyList: new(rpc.TContainerPropertyListRequest)}
	resp, err := conn.PerformRequest(req)
	plist := resp.GetPropertyList().GetList()
	for prop := range plist {
		var p TProperty
		p.Name = plist[prop].GetName()
		p.Description = plist[prop].GetDesc()
		ret = append(ret, p)
	}
	return ret, err
}

func (conn *PortoConnection) Dlist() (ret []TData, err error) {
	req := &rpc.TContainerRequest{DataList: new(rpc.TContainerDataListRequest)}
	resp, err := conn.PerformRequest(req)
	if err != nil {
		return nil, err
	}

	dlist := resp.GetDataList().GetList()
	for data := range dlist {
		var p TData
		p.Name = dlist[data].GetName()
		p.Description = dlist[data].GetDesc()
		ret = append(ret, p)
	}

	return ret, nil
}

func (conn *PortoConnection) Get(containers []string, variables []string) (ret map[string]map[string]TPortoGetResponse, err error) {
	ret = make(map[string]map[string]TPortoGetResponse)
	req := &rpc.TContainerRequest{Get: &rpc.TContainerGetRequest{Name: containers,
		Variable: variables}}
	resp, err := conn.PerformRequest(req)
	if err != nil {
		return nil, err
	}

	for i := range resp.GetGet().GetList() {
		item := resp.GetGet().GetList()[i]
		_v := item.GetKeyval()

		for j := range _v {
			var v TPortoGetResponse
			v.Value = _v[j].GetValue()
			v.Error = int(_v[j].GetError())
			v.ErrorMsg = _v[j].GetErrorMsg()
			if ret[item.GetName()] == nil {
				ret[item.GetName()] = make(map[string]TPortoGetResponse)
			}
			ret[item.GetName()][_v[j].GetVariable()] = v
		}
	}
	return ret, err
}

func (conn *PortoConnection) GetProperty(name string, property string) (string, error) {
	req := &rpc.TContainerRequest{GetProperty: &rpc.TContainerGetPropertyRequest{
		Name: &name, Property: &property}}
	resp, err := conn.PerformRequest(req)
	if err != nil {
		return "", err
	}

	return resp.GetGetProperty().GetValue(), nil
}

func (conn *PortoConnection) SetProperty(name string, property string, value string) error {
	req := &rpc.TContainerRequest{SetProperty: &rpc.TContainerSetPropertyRequest{
		Name: &name, Property: &property, Value: &value}}
	_, err := conn.PerformRequest(req)
	return err
}

func (conn *PortoConnection) GetData(name string, data string) (string, error) {
	req := &rpc.TContainerRequest{GetData: &rpc.TContainerGetDataRequest{
		Name: &name, Data: &data}}
	resp, err := conn.PerformRequest(req)
	if err != nil {
		return "", err
	}

	return resp.GetGetData().GetValue(), nil
}

// VolumeAPI
func (conn *PortoConnection) ListVolumeProperties() (ret []TProperty, err error) {
	req := &rpc.TContainerRequest{ListVolumeProperties: &rpc.TVolumePropertyListRequest{}}
	resp, err := conn.PerformRequest(req)
	for i := range resp.GetVolumePropertyList().GetProperties() {
		prop := resp.GetVolumePropertyList().GetProperties()[i]
		var desc TProperty
		desc.Name = prop.GetName()
		desc.Description = prop.GetDesc()
		ret = append(ret, desc)
	}
	return ret, err
}

func (conn *PortoConnection) CreateVolume(path string, config map[string]string) (desc TVolumeDescription, err error) {
	var properties []*rpc.TVolumeProperty
	for k, v := range config {
		prop := &rpc.TVolumeProperty{Name: &k, Value: &v}
		properties = append(properties, prop)
	}
	req := &rpc.TContainerRequest{CreateVolume: &rpc.TVolumeCreateRequest{
		Path: &path, Properties: properties}}
	resp, err := conn.PerformRequest(req)
	volume := resp.GetVolume()
	desc.Path = volume.GetPath()
	for i := range volume.GetContainers() {
		desc.Containers = append(desc.Containers, volume.GetContainers()[i])
	}
	desc.Properties = make(map[string]string)
	for i := range volume.GetProperties() {
		k := volume.GetProperties()[i].GetName()
		v := volume.GetProperties()[i].GetValue()
		desc.Properties[k] = v
	}
	return desc, err
}

func (conn *PortoConnection) LinkVolume(path string, container string) error {
	req := &rpc.TContainerRequest{LinkVolume: &rpc.TVolumeLinkRequest{
		Path: &path, Container: &container}}
	_, err := conn.PerformRequest(req)
	return err
}

func (conn *PortoConnection) UnlinkVolume(path string, container string) error {
	req := &rpc.TContainerRequest{UnlinkVolume: &rpc.TVolumeUnlinkRequest{
		Path: &path, Container: &container}}
	_, err := conn.PerformRequest(req)
	return err
}

func (conn *PortoConnection) ListVolumes(path string, container string) (ret []TVolumeDescription, err error) {
	req := &rpc.TContainerRequest{ListVolumes: &rpc.TVolumeListRequest{
		Path: &path, Container: &container}}
	if path == "" {
		req.ListVolumes.Path = nil
	}
	if container == "" {
		req.ListVolumes.Container = nil
	}
	resp, err := conn.PerformRequest(req)
	for i := range resp.GetVolumeList().GetVolumes() {
		var desc TVolumeDescription
		volume := resp.GetVolumeList().GetVolumes()[i]
		desc.Path = volume.GetPath()
		for i := range volume.GetContainers() {
			desc.Containers = append(desc.Containers, volume.GetContainers()[i])
		}
		desc.Properties = make(map[string]string)
		for i := range volume.GetProperties() {
			k := volume.GetProperties()[i].GetName()
			v := volume.GetProperties()[i].GetValue()
			desc.Properties[k] = v
		}
		ret = append(ret, desc)
	}
	return ret, err
}

// LayerAPI
func (conn *PortoConnection) ImportLayer(layer string, tarball string, merge bool) error {
	req := &rpc.TContainerRequest{ImportLayer: &rpc.TLayerImportRequest{
		Layer: &layer, Tarball: &tarball, Merge: &merge}}
	_, err := conn.PerformRequest(req)
	return err
}

func (conn *PortoConnection) ExportLayer(volume string, tarball string) error {
	req := &rpc.TContainerRequest{ExportLayer: &rpc.TLayerExportRequest{
		Volume: &volume, Tarball: &tarball}}
	_, err := conn.PerformRequest(req)
	return err
}

func (conn *PortoConnection) RemoveLayer(layer string) error {
	req := &rpc.TContainerRequest{RemoveLayer: &rpc.TLayerRemoveRequest{
		Layer: &layer}}
	_, err := conn.PerformRequest(req)
	return err
}

func (conn *PortoConnection) ListLayers() ([]string, error) {
	req := &rpc.TContainerRequest{ListLayers: &rpc.TLayerListRequest{}}
	resp, err := conn.PerformRequest(req)
	if err != nil {
		return nil, err
	}

	return resp.GetLayers().GetLayer(), nil
}
