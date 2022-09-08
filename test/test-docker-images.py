import porto
from test_common import *
import subprocess

IMAGE_NAME = "alpine:3.16.2"
IMAGE_FULL_NAME = "registry-1.docker.io/library/alpine:3.16.2@9c6f0724472873bb50a2ae67a9e7adcb57673a183cea8b06eb778dca859181b5"
LAYER_NAME = "alpine"

K8S_IMAGE_NAME = "k8s.gcr.io/pause:3.7"
K8S_IMAGE_FULL_NAME = "k8s.gcr.io/pause:3.7@221177c6082a88ea4f6240ab2450d540955ac6f4d5454f0e15751b653ebda165"

conn = porto.Connection(timeout=30)

ConfigurePortod('docker-images', """
daemon {
    docker_images_support: true
}
""")

# api
image = conn.PullDockerImage(IMAGE_NAME)
ExpectEq(image.full_name, IMAGE_FULL_NAME)

images = conn.ListDockerImages()
Expect(image in images)

images = conn.ListDockerImages(mask=IMAGE_FULL_NAME[:-32]+"***")
ExpectEq(len(images), 1)
ExpectEq(image.full_name, images[0].full_name)

image = conn.DockerImageStatus(IMAGE_NAME)
ExpectEq(image.full_name, IMAGE_FULL_NAME)

conn.RemoveDockerImage(IMAGE_NAME)
ExpectException(conn.DockerImageStatus, porto.exceptions.DockerImageNotFound, IMAGE_NAME)


# volume
image = conn.PullDockerImage(IMAGE_NAME)
volume = conn.CreateVolume(None, image=IMAGE_NAME, layers=[LAYER_NAME])
ExpectEq(volume.GetProperty("image"), IMAGE_NAME)
Expect(LAYER_NAME not in volume.GetProperty("layers").split(";"))

conn.RemoveDockerImage(IMAGE_NAME)
ExpectException(conn.DockerImageStatus, porto.exceptions.DockerImageNotFound, IMAGE_NAME)


# portoctl
image = subprocess.check_output([portoctl, "docker-pull", IMAGE_NAME]).decode("utf-8")[:-1]
ExpectEq(image, IMAGE_FULL_NAME)

images = subprocess.check_output([portoctl, "docker-images"]).decode("utf-8").split()
Expect(image in images)

images = subprocess.check_output([portoctl, "docker-images", IMAGE_FULL_NAME[:-32]+"***"]).decode("utf-8") .split()
ExpectEq(len(images), 1)
ExpectEq(image, images[0])

image = subprocess.check_output([portoctl, "docker-rmi", IMAGE_NAME]).decode("utf-8")
ExpectEq(image, "")


# k8s image
image = conn.PullDockerImage(K8S_IMAGE_NAME)
ExpectEq(image.full_name, K8S_IMAGE_FULL_NAME)

image = conn.DockerImageStatus(K8S_IMAGE_NAME)
ExpectEq(image.full_name, K8S_IMAGE_FULL_NAME)

conn.RemoveDockerImage(K8S_IMAGE_NAME)
ExpectException(conn.DockerImageStatus, porto.exceptions.DockerImageNotFound, K8S_IMAGE_NAME)
