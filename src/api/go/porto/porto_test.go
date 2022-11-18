package porto

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
)

const (
	testTmpDir    = "/tmp/"
	testContainer = "golang_testContainer"
	testVolume    = testTmpDir + "golang_testVolume"
	testLayer     = "golang_testLayer"
	testTarball   = testTmpDir + testLayer + ".tgz"
	testStorage   = "go_abcde"
	testPlace     = testTmpDir + "golang_place"
)

func FailOnError(t *testing.T, c API, err error) {
	if err != nil {
		t.Error(err)
	}
}

func ConnectToPorto(t *testing.T) API {
	c, err := Connect()
	if c == nil {
		t.Error(err)
		t.FailNow()
	}
	return c
}

func makeTestContainerName(t *testing.T) string {
	return testContainer + "_" + t.Name()
}

func TestGetVersion(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()
	maj, min, err := c.GetVersion()
	FailOnError(t, c, err)
	t.Logf("Porto version %s.%s", maj, min)
}

func TestPlist(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()
	plist, err := c.Plist()
	FailOnError(t, c, err)
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
	c := ConnectToPorto(t)
	defer c.Close()
	dlist, err := c.Dlist()
	FailOnError(t, c, err)
	for i := range dlist {
		if dlist[i].Name == "state" {
			t.Logf("Porto supports state data: %s", dlist[i].Description)
			return
		}
	}
	t.Error("Porto doesn't support state data")
	t.FailNow()
}

func TestCreateWeak(t *testing.T) {
	c := ConnectToPorto(t)
	FailOnError(t, c, c.CreateWeak(testContainer))
	c.Close()

	c = ConnectToPorto(t)
	defer c.Close()
	err := c.Destroy(testContainer)
	if err == nil {
		t.Fail()
	}
}

func TestCreate(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()
	FailOnError(t, c, c.Create(testContainer))
}

func TestList(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()
	list, err := c.List()
	FailOnError(t, c, err)
	for i := range list {
		if list[i] == testContainer {
			return
		}
	}
	t.Error("Created container isn't presented in a list")
	t.FailNow()
}

func TestSetProperty(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()
	FailOnError(t, c, c.SetProperty(testContainer, "command", "sleep 10000"))
}

func TestGetProperty(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()
	value, err := c.GetProperty(testContainer, "command")
	FailOnError(t, c, err)
	if value != "sleep 10000" {
		t.Error("Got a wrong command value")
		t.FailNow()
	}
}

func TestStart(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()
	FailOnError(t, c, c.Start(testContainer))
}

func TestPause(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()
	FailOnError(t, c, c.Pause(testContainer))
}

func TestResume(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()
	FailOnError(t, c, c.Resume(testContainer))
}

func TestKill(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()
	FailOnError(t, c, c.Kill(testContainer, syscall.SIGTERM))
}

func TestWait(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()
	containers := []string{testContainer}
	container, err := c.Wait(containers, -1)
	FailOnError(t, c, err)
	if container != testContainer {
		t.Error("Wait returned a wrong container")
		t.FailNow()
	}
}

func TestGetData(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()
	value, err := c.GetData(testContainer, "state")
	FailOnError(t, c, err)
	if value != "dead" {
		t.Error("Got a wrong state value")
		t.FailNow()
	}
}

func TestGet(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()
	containers := []string{testContainer}
	variables := []string{"state", "exit_status"}
	resp, err := c.Get(containers, variables)
	FailOnError(t, c, err)
	if resp[testContainer]["state"].Value != "dead" {
		t.Error("Got a wrong state value")
		t.FailNow()
	}
	if resp[testContainer]["exit_status"].Value != "15" {
		t.Error("Got a wrong exit_status value")
		t.FailNow()
	}
}

func TestConvertPath(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()
	resp, err := c.ConvertPath("/", testContainer, testContainer)
	FailOnError(t, c, err)
	if resp != "/" {
		t.Error("Got wrong path conversion")
		t.FailNow()
	}
}

func TestStop(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()
	FailOnError(t, c, c.Stop(testContainer))
}

func TestDestroy(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()
	FailOnError(t, c, c.Destroy(testContainer))
}

// VolumeAPI
func TestListVolumeProperties(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()
	properties, err := c.ListVolumeProperties()
	FailOnError(t, c, err)
	for i := range properties {
		if properties[i].Name == "backend" {
			return
		}
	}
	t.Error("Porto doesn't list backend volume property")
	t.FailNow()
}

func TestCreateVolume(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()
	os.Remove(testVolume)
	os.Mkdir(testVolume, 0755)
	config := make(map[string]string)
	config["private"] = "golang test volume"
	_, err := c.CreateVolume(testVolume, config)
	FailOnError(t, c, err)
}

func TestListVolumes(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()
	volumes, err := c.ListVolumes("", "")
	FailOnError(t, c, err)
	for i := range volumes {
		if volumes[i].Path == testVolume {
			return
		}
	}
	t.Error("Porto doesn't list previously created volume")
	t.FailNow()
}

func TestLinkVolume(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()
	FailOnError(t, c, c.Create(testContainer))
	FailOnError(t, c, c.LinkVolume(testVolume, testContainer, "", false, false))
}

func TestLinkVolumeTarget(t *testing.T) {
	cntName := makeTestContainerName(t)
	cntDir := filepath.Join(testTmpDir, cntName)
	cntRootDir := filepath.Join(cntDir, "root")
	cntMountDir := filepath.Join(cntDir, "src")

	// Prepare container root, mount
	os.MkdirAll(cntRootDir+"/dst", 0755)
	defer os.RemoveAll(cntDir)
	os.MkdirAll(cntMountDir, 0755)

	c := ConnectToPorto(t)
	defer c.Close()

	FailOnError(t, c, c.Create(cntName))
	defer c.Destroy(cntName)
	FailOnError(t, c, c.SetProperty(cntName, "root", cntRootDir))
	_, err := c.CreateVolume(cntMountDir, map[string]string{
		"backend": "bind",
		"storage": cntMountDir,
	})
	FailOnError(t, c, err)
	FailOnError(t, c, c.LinkVolume(cntMountDir, cntName, "/dst", false, true))
	FailOnError(t, c, c.UnlinkVolume(cntMountDir, "/", ""))
	FailOnError(t, c, c.UnlinkVolume(cntMountDir, cntName, "/dst"))
}

func TestExportLayer(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()
	FailOnError(t, c, c.ExportLayer(testVolume, "/tmp/goporto.tgz"))
	os.Remove("/tmp/goporto.tgz")
}

func TestUnlinkVolume(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()
	FailOnError(t, c, c.UnlinkVolume(testVolume, testContainer, ""))
	FailOnError(t, c, c.UnlinkVolume(testVolume, "/", ""))
	FailOnError(t, c, c.Destroy(testContainer))
	os.Remove(testVolume)
}

// LayerAPI
func TestImportLayer(t *testing.T) {
	// Prepare tarball
	os.Mkdir("/tmp/golang_testTarball", 0755)
	defer os.Remove("/tmp/golang_testTarball")
	os.Mkdir("/tmp/golang_testTarball/dir", 0755)
	defer os.Remove("/tmp/golang_testTarball/dir")

	cmd := exec.Command("tar", "-czf", testTarball,
		"/tmp/golang/testTarball")
	err := cmd.Start()
	if err != nil {
		t.Error("Can't prepare test tarball")
		t.SkipNow()
	}
	err = cmd.Wait()
	defer os.Remove(testTarball)

	// Import
	c := ConnectToPorto(t)
	defer c.Close()

	FailOnError(t, c,
		c.ImportLayer(testLayer, testTarball, false))
}

func TestLayerPrivate(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()

	FailOnError(t, c, c.SetLayerPrivate(testLayer, "", "456"))
	private, err := c.GetLayerPrivate(testLayer, "")
	FailOnError(t, c, err)

	if private == "456" {
		return
	}

	t.Error("Can't get/set new layer private")
	t.FailNow()
}

func TestListLayers(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()
	layers, err := c.ListLayers()
	FailOnError(t, c, err)
	for i := range layers {
		if layers[i] == testLayer {
			return
		}
	}
	t.Error("Porto doesn't list previously imported layer")
	t.FailNow()
}

func TestListLayers2(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()
	layers, err := c.ListLayers2("", "")
	FailOnError(t, c, err)
	for i := range layers {
		if layers[i].Name == testLayer &&
			layers[i].PrivateValue == "456" &&
			layers[i].LastUsage < 10 {

			return
		}
	}
	t.Error("Porto doesn't list previously imported layer as object")
	t.FailNow()
}

func TestRemoveLayer(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()
	FailOnError(t, c, c.RemoveLayer(testLayer))
}

func TestCreateStorage(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()

	config := make(map[string]string)
	config["backend"] = "native"
	config["storage"] = testStorage
	config["private"] = "12345"

	volume, err := c.CreateVolume("", config)
	FailOnError(t, c, err)
	FailOnError(t, c, c.UnlinkVolume3(volume.Path, "", "", false))
}

func TestListStorage(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()

	storages, err := c.ListStorage("", "")
	FailOnError(t, c, err)

	for i := range storages {
		if storages[i].Name == testStorage &&
			storages[i].PrivateValue == "12345" {

			return
		}
	}

	t.Error("Failed listing storages")
	t.FailNow()
}

func TestRemoveStorage(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()
	FailOnError(t, c, c.RemoveStorage(testStorage, ""))
}

func TestPlace(t *testing.T) {
	c := ConnectToPorto(t)
	defer c.Close()

	os.RemoveAll(testPlace)
	os.Mkdir(testPlace, 0755)
	os.Mkdir(testPlace+"/porto_volumes", 0755)
	os.Mkdir(testPlace+"/porto_layers", 0755)
	os.Mkdir(testPlace+"/porto_storage", 0755)

	config := make(map[string]string)
	config["private"] = "golang test volume"
	config["place"] = testPlace
	config["storage"] = "abcd"

	volume, err := c.CreateVolume("", config)
	FailOnError(t, c, err)
	if !strings.Contains(volume.Path, testPlace) {
		t.Error("Volume does not use desired place")
		t.FailNow()
	}

	_, err = os.Stat(volume.Path)
	FailOnError(t, c, err)

	storages, err := c.ListStorage(testPlace, "")
	FailOnError(t, c, err)

	if len(storages) == 0 || storages[0].Name != "abcd" {
		t.Error("Storage failed to be created in place")
		t.FailNow()
	}

	FailOnError(t, c, c.UnlinkVolume3(volume.Path, "", "", false))
	FailOnError(t, c, c.RemoveStorage("abcd", testPlace))
	FailOnError(t, c, os.Remove(testPlace+"/porto_volumes"))
	FailOnError(t, c, os.Remove(testPlace+"/porto_layers"))
	FailOnError(t, c, os.Remove(testPlace+"/porto_storage"))
	FailOnError(t, c, os.RemoveAll(testPlace+"/porto_docker"))
	FailOnError(t, c, os.Remove(testPlace))
}
