package porto

import (
	"os"
	"os/exec"
	"testing"
	"porto/rpc"
)

func FailOnError(t *testing.T, conn *PortoConnection, err error) {
	if err == nil {
		if conn != nil {
			if conn.GetLastError() != 0 {
				t.Error("GetLastError() return value isn't zero")
				t.FailNow()
			}
			if conn.GetLastErrorMessage() != "" {
				t.Error("GetLastErrorMesage() return value isn't empty")
				t.FailNow()
			}
		}
	} else {
		if conn != nil && conn.GetLastError() != 0 {
			t.Errorf("error %s (%s)",
				rpc.EError_name[int32(conn.GetLastError())],
				conn.GetLastErrorMessage())
		} else {
			t.Error(err)
		}
	}
}

func ConnectToPorto(t *testing.T) *PortoConnection {
	conn, err := NewPortoConnection()
	if conn == nil {
		t.Error(err)
		t.FailNow()
	}
	return conn
}

const test_container string = "golang_test_container"
const test_volume string = "/tmp/golang_test_volume"
const test_layer string = "golang_test_layer"
const test_tarball = "/tmp/" + test_layer + ".tgz"

func TestGetVersion(t *testing.T) {
	conn := ConnectToPorto(t)
	defer conn.Close()
	maj, min, err := conn.GetVersion()
	FailOnError(t, conn, err)
	t.Logf("Porto version %d.%d", maj, min)
}

func TestPlist(t *testing.T) {
	conn := ConnectToPorto(t)
	defer conn.Close()
	plist, err := conn.Plist()
	FailOnError(t, conn, err)
	for i := range plist {
		if plist[i].Name == "command" {
			t.Logf("Porto supports command property: %s", plist[i].Description)
			return
		}
	}
	t.Error("Porto doesn't support command property")
	t.FailNow()
}

func TestDlist(t *testing.T) {
	conn := ConnectToPorto(t)
	defer conn.Close()
	dlist, err := conn.Dlist()
	FailOnError(t, conn, err)
	for i := range dlist {
		if dlist[i].Name == "state" {
			t.Logf("Porto supports state data: %s", dlist[i].Description)
			return
		}
	}
	t.Error("Porto doesn't support state data")
	t.FailNow()
}

func TestCreate(t *testing.T) {
	conn := ConnectToPorto(t)
	defer conn.Close()
	FailOnError(t, conn, conn.Create(test_container))
}

func TestList(t *testing.T) {
	conn := ConnectToPorto(t)
	defer conn.Close()
	list, err := conn.List()
	FailOnError(t, conn, err)
	for i := range list {
		if list[i] == test_container {
			return
		}
	}
	t.Error("Created container isn't presented in a list")
	t.FailNow()
}

func TestSetProperty(t *testing.T) {
	conn := ConnectToPorto(t)
	defer conn.Close()
	FailOnError(t, conn, conn.SetProperty(test_container, "command", "sleep 10000"))
}

func TestGetProperty(t *testing.T) {
	conn := ConnectToPorto(t)
	defer conn.Close()
	value, err := conn.GetProperty(test_container, "command")
	FailOnError(t, conn, err)
	if value != "sleep 10000" {
		t.Error("Got a wrong command value")
		t.FailNow()
	}
}

func TestStart(t *testing.T) {
	conn := ConnectToPorto(t)
	defer conn.Close()
	FailOnError(t, conn, conn.Start(test_container))
}

func TestPause(t *testing.T) {
	conn := ConnectToPorto(t)
	defer conn.Close()
	FailOnError(t, conn, conn.Pause(test_container))
}

func TestResume(t *testing.T) {
	conn := ConnectToPorto(t)
	defer conn.Close()
	FailOnError(t, conn, conn.Resume(test_container))
}

func TestKill(t *testing.T) {
	conn := ConnectToPorto(t)
	defer conn.Close()
	FailOnError(t, conn, conn.Kill(test_container, 15)) // Send SIGTERM
}

func TestWait(t *testing.T) {
	conn := ConnectToPorto(t)
	defer conn.Close()
	containers := []string{test_container}
	container, err := conn.Wait(containers, -1)
	FailOnError(t, conn, err)
	if container != test_container {
		t.Error("Wait returned a wrong container")
		t.FailNow()
	}
}

func TestGetData(t *testing.T) {
	conn := ConnectToPorto(t)
	defer conn.Close()
	value, err := conn.GetData(test_container, "state")
	FailOnError(t, conn, err)
	if value != "dead" {
		t.Error("Got a wrong state value")
		t.FailNow()
	}
}

func TestGet(t *testing.T) {
	conn := ConnectToPorto(t)
	defer conn.Close()
	containers := []string{test_container}
	variables := []string{"state", "exit_status"}
	resp, err := conn.Get(containers, variables)
	FailOnError(t, conn, err)
	if resp[test_container]["state"].Value != "dead" {
		t.Error("Got a wrong state value")
		t.FailNow()
	}
	if resp[test_container]["exit_status"].Value != "15" {
		t.Error("Got a wrong exit_status value")
		t.FailNow()
	}
}

func TestStop(t *testing.T) {
	conn := ConnectToPorto(t)
	defer conn.Close()
	FailOnError(t, conn, conn.Stop(test_container))
}

func TestDestroy(t *testing.T) {
	conn := ConnectToPorto(t)
	defer conn.Close()
	FailOnError(t, conn, conn.Destroy(test_container))
}

// VolumeAPI
func TestListVolumeProperties(t *testing.T) {
	conn := ConnectToPorto(t)
	defer conn.Close()
	properties, err := conn.ListVolumeProperties()
	FailOnError(t, conn, err)
	for i := range properties {
		if properties[i].Name == "backend" {
			return
		}
	}
	t.Error("Porto doesn't list backend volume property")
	t.FailNow()
}

func TestCreateVolume(t *testing.T) {
	conn := ConnectToPorto(t)
	defer conn.Close()
	os.Remove(test_volume)
	os.Mkdir(test_volume, 0755)
	config := make(map[string]string)
	config["private"] = "golang test volume"
	_, err := conn.CreateVolume(test_volume, config)
	FailOnError(t, conn, err)
}

func TestListVolumes(t *testing.T) {
	conn := ConnectToPorto(t)
	defer conn.Close()
	volumes, err := conn.ListVolumes("", "")
	FailOnError(t, conn, err)
	for i := range volumes {
		if volumes[i].Path == test_volume {
			return
		}
	}
	t.Error("Porto doesn't list previously created volume")
	t.FailNow()
}

func TestLinkVolume(t *testing.T) {
	conn := ConnectToPorto(t)
	defer conn.Close()
	FailOnError(t, conn, conn.Create(test_container))
	FailOnError(t, conn, conn.LinkVolume(test_volume, test_container))
}

func TestExportLayer(t *testing.T) {
	conn := ConnectToPorto(t)
	defer conn.Close()
	FailOnError(t, conn, conn.ExportLayer(test_volume, "/tmp/goporto.tgz"))
	os.Remove("/tmp/goporto.tgz")
}

func TestUnlinkVolume(t *testing.T) {
	conn := ConnectToPorto(t)
	defer conn.Close()
	FailOnError(t, conn, conn.UnlinkVolume(test_volume, test_container))
	FailOnError(t, conn, conn.UnlinkVolume(test_volume, "/"))
	FailOnError(t, conn, conn.Destroy(test_container))
	os.Remove(test_volume)
}

// LayerAPI
func TestImportLayer(t *testing.T) {
	// Prepare tarball
	os.Mkdir("/tmp/golang_test_tarball", 0755)
	defer os.Remove("/tmp/golang_test_tarball")
	os.Mkdir("/tmp/golang_test_tarball/dir", 0755)
	defer os.Remove("/tmp/golang_test_tarball/dir")

	cmd := exec.Command("tar", "-czf", test_tarball,
		"/tmp/golang/test_tarball")
	err := cmd.Start()
	if err != nil {
		t.Error("Can't prepare test tarball")
		t.SkipNow()
	}
	err = cmd.Wait()
	defer os.Remove(test_tarball)

	// Import
	conn := ConnectToPorto(t)
	defer conn.Close()

	FailOnError(t, conn,
		conn.ImportLayer(test_layer, test_tarball, false))
}

func TestListLayers(t *testing.T) {
	conn := ConnectToPorto(t)
	defer conn.Close()
	layers, err := conn.ListLayers()
	FailOnError(t, conn, err)
	for i := range layers {
		if layers[i] == test_layer {
			return
		}
	}
	t.Error("Porto doesn't list previously imported layer")
	t.FailNow()
}

func TestRemoveLayer(t *testing.T) {
	conn := ConnectToPorto(t)
	defer conn.Close()
	FailOnError(t, conn, conn.RemoveLayer(test_layer))
}

