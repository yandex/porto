import porto
from test_common import *
import subprocess

IMAGE_NAME = "alpine:3.16.2"
IMAGE_TAG = "registry-1.docker.io/library/alpine:3.16.2"
IMAGE_DIGEST = "9c6f0724472873bb50a2ae67a9e7adcb57673a183cea8b06eb778dca859181b5"
LAYER_NAME = "alpine"

K8S_IMAGE_TAG = "k8s.gcr.io/pause:3.7"
K8S_IMAGE_DIGEST = "221177c6082a88ea4f6240ab2450d540955ac6f4d5454f0e15751b653ebda165"
K8S_IMAGE_ALT_TAG = "registry-1.docker.io/kndrvt/pause:latest"

conn = porto.Connection(timeout=30)

ConfigurePortod('docker-images', """
daemon {
    docker_images_support: true
}
""")

def check_image(image, digest, tags):
    ExpectEq(image.id, digest)
    ExpectEq(set(image.tags), set(tags))
    ExpectEq(len(image.digests), 1)
    ExpectEq(image.digests[0], "sha256:" + digest)


# api
print("Check python api")

image = conn.PullDockerImage(IMAGE_NAME)
check_image(image, IMAGE_DIGEST, [IMAGE_TAG])

for mask in (None, IMAGE_TAG[:-1]+"***", IMAGE_TAG):
    images = conn.ListDockerImages(mask=mask)
    Expect(image.id in list(map(lambda x: x.id, images)))
    if mask:
        ExpectEq(len(images), 1)
        check_image(images[0], IMAGE_DIGEST, [IMAGE_TAG])

for name in (IMAGE_NAME, IMAGE_TAG, IMAGE_DIGEST, IMAGE_DIGEST[:12]):
    image = conn.DockerImageStatus(IMAGE_DIGEST[:12])
    check_image(image, IMAGE_DIGEST, [IMAGE_TAG])

for name in (IMAGE_NAME, IMAGE_TAG, IMAGE_DIGEST, IMAGE_DIGEST[:12]):
    image = conn.PullDockerImage(IMAGE_NAME)
    conn.RemoveDockerImage(name)
    ExpectException(conn.DockerImageStatus, porto.exceptions.DockerImageNotFound, name)


# volume
print("Check volumes")

image = conn.PullDockerImage(IMAGE_NAME)
volume = conn.CreateVolume(None, image=IMAGE_NAME, layers=[LAYER_NAME])
ExpectEq(volume.GetProperty("image"), IMAGE_NAME)
Expect(LAYER_NAME not in volume.GetProperty("layers").split(";"))

conn.RemoveDockerImage(image.id)
ExpectException(conn.DockerImageStatus, porto.exceptions.DockerImageNotFound, image.id)


# portoctl
print("Check portoctl commands")

image = subprocess.check_output([portoctl, "docker-pull", IMAGE_NAME]).decode("utf-8")[:-1]
ExpectEq(image, IMAGE_DIGEST)

for mask in ("", IMAGE_TAG[:-1]+"***", IMAGE_TAG):
    images = subprocess.check_output([portoctl, "docker-images", mask]).decode("utf-8").split()
    Expect(image[:12] in images)
    Expect(IMAGE_TAG in images)
    if mask:
        # "ID" + "NAME" + <digest> + <tag> = 4
        ExpectEq(len(images), 4)
        ExpectEq(image[:12], images[2])
        ExpectEq(IMAGE_TAG, images[3])

for name in (IMAGE_NAME, IMAGE_TAG, IMAGE_DIGEST, IMAGE_DIGEST[:12]):
    image = subprocess.check_output([portoctl, "docker-pull", IMAGE_NAME]).decode("utf-8")[:-1]
    stdout = subprocess.check_output([portoctl, "docker-rmi", name]).decode("utf-8")
    ExpectEq(stdout, "")
    ExpectException(conn.DockerImageStatus, porto.exceptions.DockerImageNotFound, image)


# k8s image
print("Check k8s pause image")

image = conn.PullDockerImage(K8S_IMAGE_TAG)
check_image(image, K8S_IMAGE_DIGEST, [K8S_IMAGE_TAG])

for name in (K8S_IMAGE_TAG, K8S_IMAGE_DIGEST, K8S_IMAGE_DIGEST[:12]):
    image = conn.DockerImageStatus(name)
    check_image(image, K8S_IMAGE_DIGEST, [K8S_IMAGE_TAG])

for name in (K8S_IMAGE_TAG, K8S_IMAGE_DIGEST, K8S_IMAGE_DIGEST[:12]):
    image = conn.PullDockerImage(K8S_IMAGE_TAG)
    conn.RemoveDockerImage(name)
    ExpectException(conn.DockerImageStatus, porto.exceptions.DockerImageNotFound, name)


# tag adding
print("Check tag adding")

conn.PullDockerImage(K8S_IMAGE_TAG)
image = conn.PullDockerImage(K8S_IMAGE_ALT_TAG)
check_image(image, K8S_IMAGE_DIGEST, [K8S_IMAGE_TAG, K8S_IMAGE_ALT_TAG])

ExpectException(conn.RemoveDockerImage, porto.exceptions.Docker, K8S_IMAGE_DIGEST)
conn.RemoveDockerImage(K8S_IMAGE_TAG)
image = conn.DockerImageStatus(K8S_IMAGE_ALT_TAG)
check_image(image, K8S_IMAGE_DIGEST, [K8S_IMAGE_ALT_TAG])

conn.RemoveDockerImage(K8S_IMAGE_ALT_TAG)
ExpectException(conn.DockerImageStatus, porto.exceptions.DockerImageNotFound, K8S_IMAGE_ALT_TAG)
